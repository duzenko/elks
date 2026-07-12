#include <linuxmt/config.h>
#include <linuxmt/limits.h>
#include <linuxmt/trace.h>
#include <arch/asm-offsets.h>
#include <arch/ports.h>
#include <arch/irq.h>
#include <arch/segment.h>

#ifdef CONFIG_286_PMODE
        .arch   i286, nojumps   // PM build: 286 superset (push imm); 8086 code unaffected
#else
        .arch   i8086, nojumps
#endif
        .code16
        .text

//------------------------------------------------------------------------------
//
//      IRQ and IRQ return paths for Linux 8086
//
// The execution thread will not return from the interrupt vector trampoline.
// Instead, the address pushed in the stack will be used to get
// the interrupt number.

/*
!
!       On entry CS:IP is all we can trust
!
!       There are three possible cases to cope with
!
!       Syscall or Interrupted user mode (intr_count == 0)
!               Switch to process's kernel stack
!               Optionally, check (SS == current->t_regs.ss)
!               and panic on failure
!               On return, task switch allowed
!
!       Interrupted kernel mode, interrupted kernel task
!               or second interrupt (intr_count == 1)
!               Switch to interrupt stack
!               On return, no task switch allowed
!
!       Interrupted interrupt service routine (intr_count > 1)
!               Already using interrupt stack, keep using it
!               On return, no task switch allowed
!
!       We do all of this to avoid per process interrupt stacks and
!       related nonsense. This way we need only one dedicated int stack
!
*/
        .extern do_signal
        .extern do_IRQ
        .extern syscall
        .extern trace_begin
        .extern trace_end
        .extern tracing
        .extern check_ustack
        .extern check_istack
        .extern bh_active
        .extern do_bottom_half
        .extern schedule
        .extern panic

        .global _irqit
_irqit:
//
//      Make room
//
        push    %ds
        push    %si
        push    %di
//
//      Recover kernel data segment
//      Was pushed by the CALLF of the dynamic handler
//      TODO: BP is better for stack work
//
        mov     %sp,%si
#ifdef CONFIG_286_PMODE
        push    $SEL_KDATA      // trampoline return-CS is code alias, not usable as DS
        pop     %ds
#else
        mov     %ss:8(%si),%ds
#endif
//
//      Determine which stack to use
//
        cmpw    $1,intr_count
        jc      utask           // We were in user mode
        jz      itask           // Using a process's kernel stack
ktask:                          // Already using interrupt stack
//
//      Already using interrupt stack, keep using it
//
        sub     $8,%si          // 14 offsets less 6 already on stack
        jmp     save_regs
//
//      Using a process's kernel stack, switch to interrupt stack
//
itask:
        mov     $istack-14,%si  // 14 offsets 0-13 of SI below
        jmp     save_regs
//
//      User mode case
//
utask:
        mov     current,%si
#ifdef CHECK_SS
//
//      We were in user mode, first confirm
//      FIXME: won't work - panic requires valid SS and SS == DS!
//
        mov     %ss,%di
        cmp     TASK_USER_SS(%si),%di // entry SS = current->t_regs.ss?
        je      utask1                // User using the right stack
        mov     $ssmsg,%ax            // INVALID SS - Corrupted user stack segment
        push    %ax
        call    panic
utask1:
#endif
//
//      Switch to kernel stack
//
        add     $TASK_USER_DI,%si
//
//      Save segment, index, BP and SP registers
//
save_regs:
        incw    intr_count
        pop     (%si)           // DI
        pop     2(%si)          // SI
        pop     8(%si)          // DS
        pop     %di             // Return offset is actually a pointer to the IRQ number
        pop     %ds             // Return segment of the dynamic handler = kernel DS
#ifdef CONFIG_286_PMODE
        push    $SEL_KDATA      // force DS = SEL_KDATA (popped return-CS is code alias)
        pop     %ds
#endif
        push    %bp             // BP
        mov     %sp,10(%si)     // SP
        mov     %ss,12(%si)     // SS
        mov     %es,6(%si)      // ES
        mov     %ax,4(%si)      // orig_ax
//
//      Load new segment and SP registers
//
        mov     %si,%sp
        mov     %ds,%si
        mov     %si,%ss
        mov     %si,%es
//
//      Save remaining registers
//
        push    %dx             // DX
        push    %cx             // CX
        push    %bx             // BX
        push    %ax             // AX
#ifdef CONFIG_286_PMODE
//
//      kstack-swap first-level entry (intr_count==1) dispatch.  pt_regs stays
//      authoritative in current->t_regs for all three cases:
//      - kernel thread interrupted (entry SS = kernel DS, not idle): its live
//        frames and the CPU interrupt frame sit on kernel_stack above t_regs.sp;
//        continue BELOW them so nothing is clobbered and a schedule() from
//        here freezes handler+frames together as the task's continuation.
//      - idle interrupted: kernel_stack is FREE (sleeping tasks' frames are in
//        the image pool; idle loops on its small in-struct stack), so run the
//        handler at kernel_stack top like a user entry.  idle has a full
//        task_struct in PM builds, so its t_regs is real.
//      - user/syscall entry: the task's kernel_stack region is empty; anchor
//        at the top and copy the 6 pt_regs arg words (ax,bx,cx,dx,di,si)
//        there so sys_* read their parameters off SP.
//      Nested entries (intr_count>1) keep the istack unchanged.
//
        cmpw    $1,intr_count
        jne     9f
        mov     current,%bx
        cmp     idle_task,%bx
        je      7f              // idle: kernel_stack top (free while idle runs)
        cmpw    $SEL_KDATA,TASK_USER_SS(%bx)
        jne     7f              // user SS: anchor at kernel_stack top
        mov     TASK_USER_SP(%bx),%sp   // kthread: continue below saved BP/int frame
        jmp     9f
7:      mov     %di,%bx         // preserve IRQ# pointer across movsw
        mov     %sp,%si         // source = &current->t_regs.ax (regs just built)
        mov     $kernel_stack+KSTACK_BYTES-12,%di   // dest = top 6 words
        mov     $6,%cx
        cld
        rep     movsw           // ES=DS here; copies ax,bx,cx,dx,di,si
        mov     %bx,%di         // restore IRQ# pointer
        mov     $kernel_stack+KSTACK_BYTES-12,%sp   // SP -> copied ax on kernel_stack
9:
#endif
//
//      ds:[di] has IRQ number
//
        movb    (%di),%al
        cmpb    $IDX_SYSCALL,%al
        jne     doirq
/*
!
!       ----------PROCESS SYSCALL----------
*/
        sti
        call    check_ustack    // Check user mode stack

#ifdef CONFIG_TRACE
        call    trace_begin
#endif

        pop     %ax             // get syscall function code in AX (from kernel_stack copy in PM)
        call    syscall
#ifdef CONFIG_286_PMODE
        mov     current,%bx     // PM: retval must land in t_regs.ax; restore_regs re-anchors there
        mov     %ax,TASK_USER_AX(%bx)
#endif
        push    %ax             // syscall return value in ax

//#ifdef CONFIG_TRACE
        // strace.c must be compiled with tail optimization off to protect top of stack
        // Now always checks for kernel stack overflow
        call    trace_end       // syscall return value is top of stack
//#endif

        cmpw    $0,bh_active    // Any active bottom halfs?
        je      1f              // No
        call    do_bottom_half  // Run bottom halves
1:      call    do_signal       // process signals
        cli
        jmp     restore_regs
//
//      Done.
//

/*
!
!       ----------PROCESS INTERRUPT----------
*/
doirq:
//
//      Call the C code
//
#ifndef CONFIG_ARCH_SWAN
        sti                     // Reenable interrupts
#endif
#ifdef CONFIG_286_PMODE
        cmpw    $1,intr_count   // user IRQ runs on kernel_stack; pt_regs is in t_regs
        jne     7f
        mov     current,%bx
        add     $TASK_USER_AX,%bx
        jmp     8f
7:      mov     %sp,%bx         // nested IRQ: pt_regs is on the istack
8:
#else
        mov     %sp,%bx         // Get pointer to pt_regs
#endif
        cbw
        push    %ax             // IRQ for later

        push    %bx             // Register base
        push    %ax             // IRQ number
        call    do_IRQ          // Do the work
        pop     %ax             // Clean parameters
        pop     %bx

        pop     %ax             // Saved IRQ
        cli                     // Disable interrupts to avoid reentering ISR

//
//      Determine if hardware or software (trap) interrupt
//
#if defined(CONFIG_ARCH_IBMPC) || defined(CONFIG_ARCH_PC98)
        cmp     $16,%ax
        jge     was_trap        // Traps need no reset

#if defined(CONFIG_TIMER_INT0F) || defined(CONFIG_TIMER_INT1C)
        cmp     $7,%ax          // INT 0F (and INT 1C) also no reset
        jz      was_trap
#endif

#if defined(CONFIG_BLK_DEV_BFD) && !defined(CONFIG_ARCH_PC98) && !defined(CONFIG_286_PMODE)
        or      %ax,%ax         // Is int #0?
        jnz     do_eoi
//
//      Call original BIOS IRQ 0 (timer) ~55ms for floppy motor control
//
        decw    bios_call_cnt   // Will call bios int?
        jne     do_eoi
        movw    $5,bios_call_cnt
        pushf
        lcall   *org_irq0
        jmp     was_trap        // EOI already sent by bios int
#endif

do_eoi: cmp     $8,%ax          // Send EOI to interrupt controller(s)
        mov     $0x20,%al       // EOI
        jb      a6              // IRQ on low chip
//
//      Reset secondary 8259 if we have taken an AT rather
//      than XT irq. We also have to prod the primary controller EOI..
//
        out     %al,$PIC2_CMD   // Ack on secondary controller
        jmp     a5
a5:     jmp     a6
a6:     out     %al,$PIC1_CMD   // Ack on primary controller

#elif defined(CONFIG_ARCH_8018X)
        cmp     $16,%ax         // Determine if trap or interrupt
        jge     was_trap        // Traps need no reset
        mov $0x8000, %ax        // set the NSPEC bit on the
        mov $0xff02, %dx        // EOI register so the ICU
        out %ax, %dx            // acks the highest priority interrupt

#elif defined(CONFIG_ARCH_NECV25)
        cmp     $16,%ax         // Determine if trap or interrupt
        jge     was_trap        // Traps need no reset
        .word   0x920f          // NEC V25 specific FINT (End Of Interrupt) instruction

#elif defined(CONFIG_ARCH_SOLO86)
        mov $0x1A, %dx          // Send INT done
        out %al, %dx
#endif

was_trap:
//
//      Check if bottom halves can be run or rescheduling can occur
//

//  At this time after processing the hardware interrupt, the following actions
//  can be taken, depending on the kernel state described by intr_count:
//  intr_count = 0  not possible
//  intr_count = 1  interrupted user code (using process's kernel stack)
//      Action: Run bottom halves, schedule and do_signal.
//  intr_count = 2  interrupted kernel code or idle task (using interrupt stack)
//      Action: Run bottom halves, no rescheduling.
//  intr_count > 2  interrupted handler code (using interrupt stack)
//      Action: let interrupt stack unwind by returning immediately.
//
//  Hardware interrupts of kernel or idle code have to use the interrupt stack,
//  which means that calling schedule() cannot be allowed. After possibly running
//  BHs on the kernel's user process or interrupt stack, we will return to user mode,
//  or switch back to the interrupted kernel user process or idle task stack.
//  Interrupted kernel code will be resumed with no reschedling, and in the case
//  of idle task interruption, rescheduling will occur by the idle task when resumed.

        cmp     $2,intr_count   // Interrupting user or non-handler kernel code?
        ja      restore_regs    // No, interrupting handler code
        jne     1f              // Yes, interrupting user code
//
// Interrupted kernel or idle task code (intr_count = 2), run BHs and return
//
        cmpw    $0,bh_active    // Any active bottom halfs?
        je      3f              // No, return
        sti
        call    do_bottom_half  // Run bottom halves
        cli
        jmp     3f              // Return
//
// Interrupted user code (intr_count = 1), run BHs and reschedule
//
1:      cmpw    $0,bh_active    // Any active bottom halfs?
        je      2f              // No
        sti
        call    do_bottom_half  // Run bottom halves
2:      sti                     // Interrupts enabled while reschedule or signal checking
        call    schedule        // Task switch
        call    do_signal       // Check signals
        cli
3:

#ifdef CHECK_ISTACK
        mov     tracing,%ax
        and     $TRACE_ISTACK,%ax
        jz      4f
        sti
        call    check_istack    // Determine max interrupt stack used
        cli
4:
#endif

//
//      Restore registers and return
//
restore_regs:
        decw    intr_count
#ifdef CONFIG_286_PMODE
        jnz     6f              // returning to a nested level: pop regs from its stack
        mov     current,%bx     // returning to user: re-anchor SP to current->t_regs.ax
        lea     TASK_USER_AX(%bx),%sp
6:
#endif
        pop     %ax
        pop     %bx
        pop     %cx
        pop     %dx
        pop     %di
        pop     %si
        pop     %bp             // discard orig_AX
        pop     %es
        pop     %ds
        pop     %bp             // SP
        pop     %ss
        mov     %bp,%sp
        pop     %bp             // user BP
//
//      Iret restores CS:IP and F (thus including the interrupt bit)
//
        iret

/*
 *      ret_from_sys_call()
 *
 *      Called by run_init_process after sys_execve for the init task /bin/init.
 *      The stack is previously setup by kfork_proc(init_task) and arch_build_stack.
 *      Also called by arch_build_stack via do_fork when a new child process starts
 *      execution to exit the kernel to user mode.
 *
 *      ret_from_sys_call() switches to the kernel stack specified by 'current' and
 *      returns into user mode with AX=0. It does not return to the caller.
 */

        .global ret_from_syscall
ret_from_syscall:
        mov     current,%bx           // Ensure we have the
        lea     TASK_USER_BX(%bx),%sp // right kernel SP
        xor     %ax,%ax               // Just in case we are starting a new task
        push    %ax
        cli
#ifdef CONFIG_286_PMODE
//      A task's first return to user arrives here from kernel-thread context
//      (intr_count 0), so the decw in restore_regs would drift the count to -1
//      and take the nested-exit branch instead of the t_regs re-anchor.
//      This IS a first-level exit to user mode: normalize before restoring.
        movw    $1,intr_count
#endif
        jmp     restore_regs

/*
 *      tswitch()
 *
 *      This function can only be called with SS=DS=kernel DS and
 *      CS=kernel CS. SS:SP is the relevant kernel stack. Thus we don't need
 *      to arse about with segment registers. The kernel isn't relocating.
 *
 *      tswitch() saves the "previous" task registers and state. It in effect
 *      freezes a copy of the caller context. Then restores the "current"
 *      context and returns running the current task.
 */

        .global tswitch
tswitch:
        push    %bp             // schedule()'s bp
        push    %es             // required for gcc-ia16
        push    %di
        push    %si
        mov     previous,%bx
        mov     %sp,TASK_KRNL_SP(%bx)    // previous->t_ksp = SP (offset into kernel_stack)
#ifdef CONFIG_286_PMODE
//      kstack-swap: interrupts must be off for the whole swap: an IRQ mid-copy
//      would run bottom halves that can read kernel_stack bytes (or wake tasks)
//      while the stack image is torn between two tasks.  Each task's IF state
//      is preserved in the flags it pushed before calling schedule()/tswitch.
        pushf                   // save previous task's IF on its stack (before t_ksp!)
        mov     %sp,TASK_KRNL_SP(%bx)   // re-save t_ksp including flags word
        cli
        mov     $switch_stack+SWITCHSTACK_BYTES,%sp  // neutral stack for the copy
        call    kstack_swap             // save(previous) + restore(current) on kernel_stack
        mov     current,%bx
        mov     TASK_KRNL_SP(%bx),%sp    // SP = current->t_ksp
        popf                    // restore incoming task's IF
#else
        mov     current,%bx
        mov     TASK_KRNL_SP(%bx),%sp    // SP = current->t_ksp
#endif
        pop     %si
        pop     %di
        pop     %es
        pop     %bp             // BP of schedule()
        ret

// short *getsp(void) - get stack pointer
        .global getsp
getsp:
        mov     %sp,%ax
        ret

// Halt - wait for next interrupt to save CPU power
        .global idle_halt
idle_halt:
        hlt
        ret

//------------------------------------------------------------------------------
// Save BIOS IRQ 0 timer vector
// void save_timer_irq(void)
//------------------------------------------------------------------------------

        .global save_timer_irq
save_timer_irq:
        push    %es
        xor     %ax,%ax
        mov     %ax,%es         // ES -> interrupt table
        mov     $8*4,%bx        // INT 8 (IRQ 0) vector
        mov     %es:(%bx),%ax   // get the old timer intr
        mov     %ax,org_irq0
        mov     %es:2(%bx),%ax
        mov     %ax,org_irq0+2
        pop     %es
        ret

//------------------------------------------------------------------------------
// Set interrupt vector
//------------------------------------------------------------------------------
// void int_vector_set (int vect, int_proc proc, int seg);
// arg1: vector number (byte pushed as word by the C compiler)
// arg2: function offset (word)
// arg3: function segment (word)

        .global int_vector_set
int_vector_set:
        mov %sp,%bx
        mov 6(%bx),%dx  // arg3
        mov 4(%bx),%cx  // arg2
        mov 2(%bx),%bx  // arg1

        shl $1,%bx
        shl $1,%bx

        push %ds
        xor %ax,%ax
        mov %ax,%ds

        pushf
        cli
        mov %cx,0(%bx)
        mov %dx,2(%bx)
        popf

        pop %ds
        ret

        .data
        .global intr_count
        .global endistack
        .global istack
        .extern current
        .extern previous

bios_call_cnt:                  // call BIOS IRQ 0 handler every 5th interrupt
        .word   5
org_irq0:                       // original BIOS IRQ 0 vector
        .long   0
intr_count:                     // stacked interrupts count. Start with 1
        .word   1               // because init_task() is in kernel mode
#ifdef CHECK_SS
ssmsg:  .ascii "INVALID SS\0"
#endif

        .bss
        .p2align 1
endistack:
        .skip INTRSTACK_BYTES,0 // interrupt stack
istack:
