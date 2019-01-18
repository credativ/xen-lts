/******************************************************************************
 * current.h
 * 
 * Information structure that lives at the bottom of the per-cpu Xen stack.
 */

#ifndef __X86_CURRENT_H__
#define __X86_CURRENT_H__

#include <xen/config.h>
#include <xen/percpu.h>
#include <public/xen.h>
#include <asm/page.h>

struct vcpu;

struct cpu_info {
    struct cpu_user_regs guest_cpu_user_regs;
    unsigned int processor_id;
    struct vcpu *current_vcpu;
    unsigned long per_cpu_offset;
    /*
     * Of the two following fields the latter is being set to the CR3 value
     * to be used on the given pCPU for loading whenever 64-bit PV guest
     * context is being entered. A value of zero indicates no setting of CR3
     * is to be performed.
     * The former is the value to restore when re-entering Xen, if any. IOW
     * its value being zero means there's nothing to restore.
     */
    unsigned long xen_cr3;
    unsigned long pv_cr3;

    /* See asm-x86/spec_ctrl_asm.h for usage. */
    unsigned int shadow_spec_ctrl;
    uint8_t      xen_spec_ctrl;
    uint8_t      spec_ctrl_flags;

    /*
     * The following field controls copying of the L4 page table of 64-bit
     * PV guests to the per-cpu root page table on entering the guest context.
     * If set the L4 page table is being copied to the root page table and
     * the field will be reset.
     */
    bool_t       root_pgt_changed;

  /*
     * use_pv_cr3 is set in case the value of pv_cr3 is to be written into
     * CR3 when returning from an interrupt. The main use is when returning
     * from a NMI or MCE to hypervisor code where pv_cr3 was active.
     */
    bool_t       use_pv_cr3;

    /* get_stack_bottom() must be 16-byte aligned */
    unsigned long __pad_for_stack_bottom;
};

static inline struct cpu_info *get_cpu_info(void)
{
    unsigned long tos;
    __asm__ ( "and %%rsp,%0" : "=r" (tos) : "0" (~(STACK_SIZE-1)) );
    return (struct cpu_info *)(tos + STACK_SIZE) - 1;
}

#define get_current()         (get_cpu_info()->current_vcpu)
#define set_current(vcpu)     (get_cpu_info()->current_vcpu = (vcpu))
#define current               (get_current())

#define get_processor_id()    (get_cpu_info()->processor_id)
#define set_processor_id(id)  do {                                      \
    struct cpu_info *ci__ = get_cpu_info();                             \
    ci__->per_cpu_offset = __per_cpu_offset[ci__->processor_id = (id)]; \
} while (0)

#define guest_cpu_user_regs() (&get_cpu_info()->guest_cpu_user_regs)

/*
 * Get the bottom-of-stack, as stored in the per-CPU TSS. This actually points
 * into the middle of cpu_info.guest_cpu_user_regs, at the section that
 * precisely corresponds to a CPU trap frame.
 */
#define get_stack_bottom()                      \
    ((unsigned long)&get_cpu_info()->guest_cpu_user_regs.es)

/*
 * Get the bottom-of-stack, as useful for printing stack traces.  This is the
 * highest word on the stack which might be part of a stack trace, and is the
 * adjacent word to a struct cpu_info on the stack.
 */
#define get_printable_stack_bottom(sp)          \
    ((sp & (~(STACK_SIZE-1))) +                 \
     (STACK_SIZE - sizeof(struct cpu_info) - sizeof(unsigned long)))

#define reset_stack_and_jump(__fn)                                      \
    ({                                                                  \
        __asm__ __volatile__ (                                          \
            "mov %0,%%"__OP"sp;"                                        \
             "jmp %c1"                                                  \
            : : "r" (guest_cpu_user_regs()), "i" (__fn) : "memory" );   \
        unreachable();                                                  \
    })

/*
 * Schedule tail *should* be a terminal function pointer, but leave a bugframe
 * around just incase it returns, to save going back into the context
 * switching code and leaving a far more subtle crash to diagnose.
 */
#define schedule_tail(vcpu) do {                \
        (((vcpu)->arch.schedule_tail)(vcpu));   \
        BUG();                                  \
    } while (0)

/*
 * Which VCPU's state is currently running on each CPU?
 * This is not necesasrily the same as 'current' as a CPU may be
 * executing a lazy state switch.
 */
DECLARE_PER_CPU(struct vcpu *, curr_vcpu);

#endif /* __X86_CURRENT_H__ */
