/*
 * PROCFS - a minimal /proc for ELKS
 *
 * Exports per-task info the kernel-internal way ps/meminfo can't get at
 * portably: the kernel formats the data at read time, so userland needs
 * no knowledge of task_struct layout, kernel addresses, or the memory
 * model (real-mode paragraphs vs 286 protected-mode selectors).
 *
 * Layout:
 *   /proc/uptime        "seconds.centiseconds"
 *   /proc/<pid>/stat    "pid ppid pgrp session uid state tty cpu heap free size"
 *                       state is a single char (R/S/s/T/Z/E); tty is the tty
 *                       minor (0xffff = none); cpu is % usage; heap/free are
 *                       bytes; size is total code+data bytes.
 *   /proc/<pid>/cmd     full command line (argv[], space separated)
 *
 * Everything is synthesized from the inode number; no storage.  Userland needs
 * no knowledge of task_struct layout, kernel addresses or the memory model.
 */

#include <linuxmt/config.h>
#include <linuxmt/types.h>
#include <linuxmt/errno.h>
#include <linuxmt/fs.h>
#include <linuxmt/stat.h>
#include <linuxmt/sched.h>
#include <linuxmt/kernel.h>
#include <linuxmt/mm.h>
#include <linuxmt/string.h>
#include <linuxmt/fixedpt.h>
#include <linuxmt/debug.h>
#include <arch/segment.h>
#include <arch/param.h>

/* inode numbering: everything derives from the ino */
#define INO_ROOT        1
#define INO_UPTIME      2
#define INO_TASK_BASE   0x10
#define TASK_INO(idx)   (INO_TASK_BASE + ((ino_t)(idx) << 2))
#define TASK_IDX(ino)   ((int)(((ino) - INO_TASK_BASE) >> 2))
#define TASK_SUB(ino)   ((int)((ino) & 3))      /* 0 dir, 1 stat, 2 cmd */

#define PROC_BUFSIZ     80

static struct inode_operations procfs_dir_iops;
static struct inode_operations procfs_file_iops;

/* append unsigned decimal to p, return new end */
static char *utoa_cat(char *p, unsigned long v)
{
    char tmp[11];
    int n = 0;

    do {
        tmp[n++] = '0' + (int)(v % 10);
        v /= 10;
    } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

/* task state -> ps status char */
static char state_char(int st)
{
    switch (st) {
    case TASK_RUNNING:          return 'R';
    case TASK_INTERRUPTIBLE:    return 'S';
    case TASK_UNINTERRUPTIBLE:  return 's';
    case TASK_STOPPED:          return 'T';
    case TASK_ZOMBIE:           return 'Z';
    case TASK_EXITING:          return 'E';
    default:                    return '?';
    }
}

static struct task_struct *task_of(ino_t ino)
{
    int idx = TASK_IDX(ino);

    if (idx < 0 || idx >= max_tasks) return NULL;
    if (task[idx].state == TASK_UNUSED) return NULL;
    return &task[idx];
}

/* generate the file content for ino into buf, return length */
static int procfs_gen(ino_t ino, char *buf)
{
    char *p = buf;
    struct task_struct *t;

    if (ino == INO_UPTIME) {
        jiff_t j = jiffies;

        p = utoa_cat(p, j / HZ);
        *p++ = '.';
        j = (j % HZ) * 100 / HZ;
        *p++ = '0' + (int)(j / 10);
        *p++ = '0' + (int)(j % 10);
        *p++ = '\n';
        return (int)(p - buf);
    }

    if (!(t = task_of(ino))) return 0;          /* task exited: EOF */

    if (TASK_SUB(ino) == 1) {                   /* stat */
        unsigned tty_min = 0xffff;              /* 0xffff = no controlling tty */
        unsigned long size = 0;
        word_t heap = 0, freemem = 0;

        if (t->tty) tty_min = t->tty->minor;
        if (t->mm[SEG_DATA]) {                  /* heap/free only if data seg */
            heap    = (word_t)(t->t_endbrk - t->t_enddata);
            freemem = (word_t)(t->t_regs.sp - t->t_endbrk);
        }
        if (t->mm[SEG_CODE] && t->mm[SEG_DATA]) /* paragraphs -> bytes */
            size = ((unsigned long)(t->mm[SEG_CODE]->size
                                  + t->mm[SEG_DATA]->size)) << 4;

        p = utoa_cat(p, t->pid);     *p++ = ' ';
        p = utoa_cat(p, t->ppid);    *p++ = ' ';
        p = utoa_cat(p, t->pgrp);    *p++ = ' ';
        p = utoa_cat(p, t->session); *p++ = ' ';
        p = utoa_cat(p, t->uid);     *p++ = ' ';
        *p++ = state_char(t->state); *p++ = ' ';
        p = utoa_cat(p, tty_min);    *p++ = ' ';
        p = utoa_cat(p, FIXED_INT((t->average + FIXED_HALF) >> 1)); *p++ = ' ';
        p = utoa_cat(p, heap);       *p++ = ' ';
        p = utoa_cat(p, freemem);    *p++ = ' ';
        p = utoa_cat(p, size);
        *p++ = '\n';
        return (int)(p - buf);
    }

    if (TASK_SUB(ino) == 2) {                   /* cmd = full command line */
        seg_t seg = t->t_regs.ss;               /* selector in PM, paragraph in real mode */
        word_t argv_off = t->t_begstack;
        word_t argc, off;
        unsigned char c;

        if (!t->t_begstack || t->pid == 0) return 0;
        argc = peekw(argv_off, seg);            /* argc, then argv[] ptrs above it */
        while (argc-- > 0 && (p - buf) < PROC_BUFSIZ - 2) {
            argv_off += 2;
            off = peekw(argv_off, seg);
            if (!off) break;
            while ((p - buf) < PROC_BUFSIZ - 2) {
                c = peekb(off++, seg);
                if (!c) break;
                *p++ = c;
            }
            *p++ = ' ';                         /* separator */
        }
        if (p == buf) return 0;
        p[-1] = '\n';                           /* overwrite trailing space */
        return (int)(p - buf);
    }

    return 0;
}

static size_t procfs_read(struct inode *i, struct file *f, char *buf, size_t len)
{
    char kbuf[PROC_BUFSIZ];
    int cnt;

    cnt = procfs_gen(i->i_ino, kbuf);
    if (f->f_pos >= (loff_t)cnt) return 0;
    cnt -= (int)f->f_pos;
    if ((size_t)cnt > len) cnt = len;
    fmemcpyb(buf, current->t_regs.ds, kbuf + (int)f->f_pos, KERNEL_DS, cnt);
    f->f_pos += cnt;
    return cnt;
}

/*
 * readdir: one entry per call (romfs convention).
 * root dir f_pos: 0 ".", 1 "..", 2 "uptime", 3+idx = task slots.
 * task dir f_pos: 0 ".", 1 "..", 2 "stat", 3 "cmd".
 */
static int procfs_readdir(struct inode *i, struct file *f, void *dirent,
    filldir_t filldir)
{
    char name[8];
    char *p;
    int idx;
    ino_t ino = i->i_ino;

    if (ino == INO_ROOT) {
        while (1) {
            switch ((int)f->f_pos) {
            case 0:
                if (filldir(dirent, ".", 1, f->f_pos, INO_ROOT) < 0) return 0;
                f->f_pos = 1;
                return 1;
            case 1:
                if (filldir(dirent, "..", 2, f->f_pos, INO_ROOT) < 0) return 0;
                f->f_pos = 2;
                return 1;
            case 2:
                if (filldir(dirent, "uptime", 6, f->f_pos, INO_UPTIME) < 0) return 0;
                f->f_pos = 3;
                return 1;
            default:
                idx = (int)f->f_pos - 3;
                if (idx >= max_tasks) return 0;
                f->f_pos++;
                if (task[idx].state == TASK_UNUSED || task[idx].pid == 0)
                    continue;                   /* skip empty slot */
                p = utoa_cat(name, task[idx].pid);
                if (filldir(dirent, name, (size_t)(p - name),
                        f->f_pos - 1, TASK_INO(idx)) < 0) return 0;
                return 1;
            }
        }
    }

    /* per-task directory */
    switch ((int)f->f_pos) {
    case 0:
        if (filldir(dirent, ".", 1, f->f_pos, ino) < 0) return 0;
        f->f_pos = 1;
        return 1;
    case 1:
        if (filldir(dirent, "..", 2, f->f_pos, INO_ROOT) < 0) return 0;
        f->f_pos = 2;
        return 1;
    case 2:
        if (filldir(dirent, "stat", 4, f->f_pos, ino + 1) < 0) return 0;
        f->f_pos = 3;
        return 1;
    case 3:
        if (filldir(dirent, "cmd", 3, f->f_pos, ino + 2) < 0) return 0;
        f->f_pos = 4;
        return 1;
    }
    return 0;
}

static int procfs_lookup(struct inode *dir, const char *name, size_t len,
    struct inode **result)
{
    ino_t ino = 0;
    ino_t dino = dir->i_ino;
    struct super_block *sb = dir->i_sb;
    char kname[8];
    int idx;
    pid_t pid;

    if (len < sizeof(kname)) {                  /* name is in user space */
        fmemcpyb(kname, KERNEL_DS, (void *)name, current->t_regs.ds, len);
        kname[len] = 0;

        if (!strcmp(kname, "."))  ino = dino;
        else if (!strcmp(kname, "..")) ino = INO_ROOT;  /* parent is / via covered inode */
        else if (dino == INO_ROOT) {
            if (!strcmp(kname, "uptime")) ino = INO_UPTIME;
            else {                              /* decimal pid */
                pid = 0;
                for (idx = 0; kname[idx] >= '0' && kname[idx] <= '9'; idx++)
                    pid = pid * 10 + kname[idx] - '0';
                if (pid && !kname[idx]) {
                    for (idx = 0; idx < max_tasks; idx++) {
                        if (task[idx].state != TASK_UNUSED && task[idx].pid == pid) {
                            ino = TASK_INO(idx);
                            break;
                        }
                    }
                }
            }
        } else if (TASK_SUB(dino) == 0) {
            if (!strcmp(kname, "stat")) ino = dino + 1;
            else if (!strcmp(kname, "cmd")) ino = dino + 2;
        }
    }

    iput(dir);
    if (!ino) return -ENOENT;
    if (!(*result = iget(sb, ino))) return -EACCES;
    return 0;
}

static struct file_operations procfs_file_fops = {
    NULL,                       /* lseek - default */
    procfs_read,                /* read */
    NULL,                       /* write */
    NULL,                       /* readdir */
    NULL,                       /* select */
    NULL,                       /* ioctl */
    NULL,                       /* open */
    NULL                        /* release */
};

static struct file_operations procfs_dir_fops = {
    NULL,                       /* lseek - default */
    NULL,                       /* read */
    NULL,                       /* write */
    procfs_readdir,             /* readdir */
    NULL,                       /* select */
    NULL,                       /* ioctl */
    NULL,                       /* open */
    NULL                        /* release */
};

static struct inode_operations procfs_file_iops = {
    &procfs_file_fops,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL,                       /* readlink */
    NULL,                       /* followlink */
    NULL, NULL
};

static struct inode_operations procfs_dir_iops = {
    &procfs_dir_fops,
    NULL,                       /* create */
    procfs_lookup,              /* lookup */
    NULL, NULL, NULL, NULL, NULL, NULL,
    NULL,                       /* readlink */
    NULL,                       /* followlink */
    NULL, NULL
};

static void procfs_read_inode(struct inode *i)
{
    ino_t ino = i->i_ino;

    i->i_uid = i->i_gid = 0;
    i->i_nlink = 1;
    i->i_mtime = i->i_atime = i->i_ctime = 0;
    i->i_size = 0;

    if (ino == INO_ROOT || (ino >= INO_TASK_BASE && TASK_SUB(ino) == 0)) {
        i->i_mode = S_IFDIR | 0555;
        i->i_nlink = 2;
        i->i_op = &procfs_dir_iops;
    } else {
        i->i_mode = S_IFREG | 0444;
        i->i_size = PROC_BUFSIZ;                /* upper bound; reads clamp */
        i->i_op = &procfs_file_iops;
    }
}

static void procfs_put_super(struct super_block *sb)
{
    lock_super(sb);
    sb->s_dev = 0;
    unlock_super(sb);
}

static void procfs_statfs(struct super_block *s, struct statfs *sf, int flags)
{
    memset(sf, 0, sizeof(struct statfs));
}

static struct super_operations procfs_super_ops = {
    procfs_read_inode,
    NULL,                       /* write inode */
    NULL,                       /* put inode */
    procfs_put_super,
    NULL,                       /* write super */
    NULL,                       /* remount */
    procfs_statfs
};

static struct super_block *procfs_read_super(struct super_block *s, void *data,
    int silent)
{
    struct inode *i;

    lock_super(s);
    s->s_op = &procfs_super_ops;
    i = iget(s, (ino_t)INO_ROOT);
    unlock_super(s);
    if (!i) {
        printk("proc: cannot get root inode\n");
        return NULL;
    }
    s->s_mounted = i;
    return s;
}

struct file_system_type procfs_fs_type = {
    procfs_read_super,
    FST_PROC
};
