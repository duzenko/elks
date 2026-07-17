/*
 * ps.c
 * Copyright 1998 Alistair Riddoch
 * ajr@ecs.soton.ac.uk
 *
 * This file may be distributed under the terms of the GNU General Public
 * License v2, or at your option any later version.
 *
 * This is a small version of ps for use in the ELKS project.
 * Enhanced by Greg Haerr 17 Apr 2020
 *
 * Reads /proc (procfs) when it is mounted -- the kernel formats every field,
 * so no task_struct layout, kernel addresses or memory-model knowledge is
 * needed; this is the only path that works under 286 protected mode, where
 * the /dev/kmem (seg<<4)+off protocol is ambiguous.  When procfs is absent
 * (CONFIG_PROC_FS off, the default), it falls back to the original /dev/kmem
 * implementation, which still works in real mode.
 */
#define __KERNEL__
#include <linuxmt/ntty.h>       /* for struct tty */
#undef __KERNEL__

#include <autoconf.h>           /* for CONFIG_ options */
#include <linuxmt/mm.h>
#include <linuxmt/mem.h>
#include <linuxmt/major.h>
#include <linuxmt/kdev_t.h>
#include <linuxmt/sched.h>
#include <linuxmt/fixedpt.h>
#include <arch/irq.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <dirent.h>
#include <pwd.h>
#include <getopt.h>
#include <paths.h>
#include <libgen.h>

#define LINEARADDRESS(off, seg)     ((off_t) (((off_t)seg << 4) + off))
#define MK_FP(seg,off) ((void __far *)((((unsigned long)(seg))<<16) | ((unsigned)(off))))

static int maxtasks;

/* fast cached version of devname() - scans /dev, shared by both paths */
char *dev_name(unsigned int minor)
{
    struct dirent *d;
    dev_t ttydev = MKDEV(TTY_MAJOR, minor);
    static dev_t prevdev = -1;
    static DIR *fp = NULL;
    struct stat st;
    static char path[MAXNAMLEN+6] = _PATH_DEVSL;    /* /dev/ */
#define NAMEOFF     (sizeof(_PATH_DEVSL) - 1)

    if (prevdev == ttydev) return path+NAMEOFF+3;
    if (!fp) {
        if (!(fp = opendir(_PATH_DEV)))
            return "??";
    } else rewinddir(fp);

    while ((d = readdir(fp)) != 0) {
        if (d->d_name[0] == '.')
            continue;
        if (strncmp(d->d_name, "tty", 3))
            continue;
        strcpy(&path[NAMEOFF], d->d_name);
        if (!stat(path, &st) && st.st_rdev == ttydev) {
            prevdev = ttydev;
            return path+NAMEOFF+3;
        }
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* /proc path (preferred): kernel formats every field                 */
/* ------------------------------------------------------------------ */

/* read a whole (small) proc file into buf, NUL-terminate, return len or -1 */
static int read_file(const char *path, char *buf, int size)
{
    int fd, n;

    if ((fd = open(path, O_RDONLY)) < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

static int ps_proc(int f_listall, int f_uptime, const char *progname)
{
    DIR *proc;
    struct dirent *de;
    struct passwd *pwent;
    char path[32];
    char buf[128];

    if (f_uptime) {
        unsigned long n;
        int days, hours, minutes;

        if (read_file("/proc/uptime", buf, sizeof(buf)) < 0) {
            fprintf(stderr, "%s: cannot read /proc/uptime\n", progname);
            return 1;
        }
        n = strtoul(buf, NULL, 10);             /* whole seconds */
        days = n / (24 * 3600L);
        n %= 24 * 3600L;
        hours = n / 3600L;
        n %= 3600;
        minutes = n / 60;

        printf("up for %d days, %d hour%s, and %d minute%s\n",
            days, hours, hours == 1? "": "s", minutes, minutes == 1? "": "s");
        return 0;
    }

    if (!(proc = opendir("/proc"))) {
        fprintf(stderr, "%s: cannot open /proc\n", progname);
        return 1;
    }

    printf("  PID");
    if (f_listall) printf("  PPID");
    printf("   GRP  TTY USER STAT CPU  HEAP  FREE   SIZE COMMAND\n");

    while ((de = readdir(proc)) != NULL) {
        char *s;
        int pid, ppid, pgrp, session, uid, cpu;
        char state;
        unsigned int tty, heap, freemem;
        long size;
        char *ttyp;

        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;                           /* only numeric pid dirs */

        sprintf(path, "/proc/%s/stat", de->d_name);
        if (read_file(path, buf, sizeof(buf)) <= 0)
            continue;                           /* raced with exit */

        /* "pid ppid pgrp session uid state tty cpu heap free size" */
        s = buf;
        pid     = strtol(s, &s, 10);
        ppid    = strtol(s, &s, 10);
        pgrp    = strtol(s, &s, 10);
        session = strtol(s, &s, 10);
        uid     = strtol(s, &s, 10);
        while (*s == ' ') s++;
        state   = *s++;
        tty     = strtoul(s, &s, 10);
        cpu     = strtol(s, &s, 10);
        heap    = strtoul(s, &s, 10);
        freemem = strtoul(s, &s, 10);
        size    = strtol(s, &s, 10);
        (void)session;

        ttyp = (tty == 0xffff) ? "" : dev_name(tty);
        pwent = getpwuid(uid);

        printf("%5d", pid);
        if (f_listall) printf(" %5d", ppid);
        printf(" %5d %4s %-8s%c %3d %5u %5u %6ld ",
            pgrp, ttyp, (pwent ? pwent->pw_name : "unknown"), state,
            cpu, heap, freemem, size);

        sprintf(path, "/proc/%s/cmd", de->d_name);
        if (read_file(path, buf, sizeof(buf)) > 0) {
            if ((s = strchr(buf, '\n')) != NULL) *s = '\0';
            printf("%s", buf);
        }
        printf("\n");
    }
    closedir(proc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* /dev/kmem path (fallback when procfs is not mounted)               */
/* ------------------------------------------------------------------ */

int memread(int fd, word_t off, word_t seg, void *buf, int size)
{
    if (lseek(fd, LINEARADDRESS(off, seg), SEEK_SET) == -1)
        return 0;

    if (read(fd, buf, size) != size)
        return 0;

    return 1;
}

word_t getword(int fd, word_t off, word_t seg)
{
    word_t word;

    if (!memread(fd, off, seg, &word, sizeof(word)))
        return 0;
    return word;
}

void process_name(int fd, unsigned int off, unsigned int seg)
{
    word_t argc, argv;
    char buf[80];

    argc = getword(fd, off, seg);

    while (argc-- > 0) {
        off += 2;
        argv = getword(fd, off, seg);
        if (!memread(fd, argv, seg, buf, sizeof(buf)))
            return;
        printf("%s ",buf);
    }
}

char *tty_name(int fd, unsigned int off, unsigned int seg)
{
    off_t addr = ((off_t)seg << 4) + off;
    struct tty tty;

    if (off == 0)
        return "";
    if (lseek(fd, addr, SEEK_SET) == -1) return "?";

    if (read(fd, &tty, sizeof(tty)) != sizeof(tty)) return "?";

    return dev_name(tty.minor);
}

static int ps_kmem(int f_listall, int f_uptime, const char *progname)
{
    int c, fd;
    unsigned int j, ds, off;
    word_t cseg, dseg;
    struct passwd * pwent;
    struct task_struct task_table;

    if ((fd = open("/dev/kmem", O_RDONLY)) < 0) {
        printf("no /dev/kmem\n");
        return 1;
    }
    if (ioctl(fd, MEM_GETDS, &ds) < 0 ||
        ioctl(fd, MEM_GETMAXTASKS, &maxtasks) < 0) {
        printf("no mem_getds\n");
        return 1;
    }

    if (f_uptime) {
        jiff_t uptime;
        unsigned int upoff;

        if (ioctl(fd, MEM_GETUPTIME, &upoff) < 0) {
            printf("no mem_getuptime\n");
            return 1;
        }
        jiff_t __far *puptime = MK_FP(ds, upoff);
        clr_irq();
        uptime = *puptime;
        set_irq();

        unsigned long n = uptime / HZ;
        int days = n / (24 * 3600L);
        n = n % (24 * 3600L);
        int hours = n / 3600L;
        n %= 3600;
        int minutes = n / 60 ;

        printf("up for %d days, %d hour%s, and %d minute%s\n",
            days, hours, hours == 1? "": "s", minutes, minutes == 1? "": "s");
        return 0;
    }

    if (ioctl(fd, MEM_GETTASK, &off) < 0) {
        printf("no mem_gettask\n");
        return 1;
    }

    printf("  PID");
    if (f_listall) printf("  PPID");
    printf("   GRP  TTY USER STAT ");
    printf(f_listall? "TSK": "CPU");
    printf(" ");
    if (f_listall) printf("CSEG DSEG ");
    printf(" HEAP  FREE   SIZE COMMAND\n");
    for (j = 0; j < maxtasks; j++) {
        if (!memread(fd, off + j*sizeof(struct task_struct), ds, &task_table, sizeof(task_table))) {
            printf("no memread\n");
            return 1;
        }

        switch (task_table.state) {
        case TASK_UNUSED:           continue;
        case TASK_RUNNING:          c = 'R'; break;
        case TASK_INTERRUPTIBLE:    c = 'S'; break;
        case TASK_UNINTERRUPTIBLE:  c = 's'; break;
        case TASK_STOPPED:          c = 'T'; break;
        case TASK_ZOMBIE:           c = 'Z'; break;
        case TASK_EXITING:          c = 'E'; break;
        default:                    c = '?'; break;
        }

        if (task_table.kstack_magic != KSTACK_MAGIC) {
            printf("Recompile ps, mismatched task structure\n");
            return 1;
        }

        pwent = getpwuid(task_table.uid);

        /* pid grp tty user stat*/
        printf("%5d", task_table.pid);
        if (f_listall) printf(" %5d", task_table.ppid);
        printf(" %5d %4s %-8s%c ",
                task_table.pgrp,
                tty_name(fd, (unsigned int)task_table.tty, ds),
                (pwent ? pwent->pw_name : "unknown"), c);

        if (f_listall)
            printf("%3d ", j);
        else {
            /* Round up, then divide by 2 for %. Change if SAMP_FREQ not 2 */
            unsigned long cpu_percent = (task_table.average + FIXED_HALF) >> 1;
            printf("%3d", FIXED_INT(cpu_percent));
        }

        /* CSEG*/
        cseg = (word_t)task_table.mm[SEG_CODE];
        if (f_listall) printf(" %4x ",
            cseg? getword(fd, (word_t)cseg+offsetof(struct segment, base), ds): 0);

        /* DSEG*/
        dseg = (word_t)task_table.mm[SEG_DATA];
        if (f_listall) printf("%4x",
            dseg? getword(fd, (word_t)dseg+offsetof(struct segment, base), ds): 0);

        if (dseg) {
            /* heap*/
            printf(" %5u ", (word_t)(task_table.t_endbrk - task_table.t_enddata));

            /* free*/
            printf("%5u ", (word_t)(task_table.t_regs.sp - task_table.t_endbrk));

            /* size*/
            segext_t size = getword(fd, (word_t)cseg+offsetof(struct segment, size), ds)
                            + getword(fd, (word_t)dseg+offsetof(struct segment, size), ds);
            printf("%6ld ", (long)size << 4);

            process_name(fd, task_table.t_begstack, task_table.t_regs.ss);
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    int c, pfd;
    int f_listall = 0;
    int f_uptime;
    char *progname;

    if ((progname = strrchr(argv[0], '/')) != NULL)
        progname++;
    else progname = argv[0];
    f_uptime = !strcmp(progname, "uptime");
    while ((c = getopt(argc, argv, "lu")) != -1) {
        switch (c) {
        case 'l':       /* list all */
            f_listall = 1;
            break;
        case 'u':       /* uptime */
            f_uptime = 1;
            break;
        default:
            printf("Usage: %s: [-lu]\n", progname);
            return 1;
        }
    }

    /*
     * Prefer procfs; fall back to /dev/kmem when it is not mounted
     * (CONFIG_PROC_FS off).  /proc/uptime exists iff procfs is mounted.
     */
    if ((pfd = open("/proc/uptime", O_RDONLY)) >= 0) {
        close(pfd);
        return ps_proc(f_listall, f_uptime, progname);
    }
    return ps_kmem(f_listall, f_uptime, progname);
}
