#ifndef __ASM_MSR_H
#define __ASM_MSR_H

#include "msr-index.h"

#ifndef __ASSEMBLY__

#include <xen/types.h>
#include <xen/percpu.h>
#include <xen/errno.h>
#include <asm/asm_defns.h>

#define rdmsr(msr,val1,val2) \
     __asm__ __volatile__("rdmsr" \
			  : "=a" (val1), "=d" (val2) \
			  : "c" (msr))

#define rdmsrl(msr,val) do { unsigned long a__,b__; \
       __asm__ __volatile__("rdmsr" \
			    : "=a" (a__), "=d" (b__) \
			    : "c" (msr)); \
       val = a__ | ((u64)b__<<32); \
} while(0)

#define wrmsr(msr,val1,val2) \
     __asm__ __volatile__("wrmsr" \
			  : /* no outputs */ \
			  : "c" (msr), "a" (val1), "d" (val2))

static inline void wrmsrl(unsigned int msr, __u64 val)
{
        __u32 lo, hi;
        lo = (__u32)val;
        hi = (__u32)(val >> 32);
        wrmsr(msr, lo, hi);
}

#if !defined(__x86_64__)
#define pvnested 0
static inline void pvnested_rdmsrl(uint32_t msr, uint64_t *value) { }
static inline void pvnested_wrmsrl(uint32_t msr, uint64_t value) { }
#endif  /* __x86_64__ */

extern bool_t pv_msr;

#define pv_rdmsrl(msr, var, vcpu) do {          \
        if (!pv_msr)                            \
            rdmsrl(msr, var);                   \
        else if (ax_present)                    \
            ax_vmcs_x_rdmsrl(vcpu, msr, &var);  \
        else if (pvnested)                      \
            pvnested_rdmsrl(msr, &var);         \
        else                                    \
            var = 0;                            \
    } while (0)

#define pv_rdmsr(msr, low, high, vcpu) do {     \
        uint64_t val = 0;                       \
        pv_rdmsrl(msr, val, vcpu);              \
        low = (uint32_t)val;                    \
        high = (uint32_t)(val >> 32);           \
        (void)low; (void)high;                  \
    } while (0)

#define pv_wrmsrl(msr, val, vcpu) do {          \
        if (!pv_msr)                            \
            wrmsrl(msr, val);                   \
        else if (ax_present)                    \
            ax_vmcs_x_wrmsrl(vcpu, msr, val);   \
        else if (pvnested)                      \
            pvnested_wrmsrl(msr, val);          \
    } while (0)

/* rdmsr with exception handling */
#define _rdmsr_safe(msr,val) ({\
    int _rc; \
    uint32_t lo, hi; \
    __asm__ __volatile__( \
        "1: rdmsr\n2:\n" \
        ".section .fixup,\"ax\"\n" \
        "3: xorl %0,%0\n; xorl %1,%1\n" \
        "   movl %5,%2\n; jmp 2b\n" \
        _ASM_PREVIOUS "\n" \
        _ASM_EXTABLE(1b, 3b) \
        : "=a" (lo), "=d" (hi), "=&r" (_rc) \
        : "c" (msr), "2" (0), "i" (-EFAULT)); \
    (val) = lo | ((uint64_t)hi << 32); \
    _rc; })
#ifdef UXEN_HOST_OSX
#define rdmsr_safe(msr, val) UI_HOST_CALL(ui_rdmsr_safe, msr, &val)
#else  /* UXEN_HOST_OSX */
#define rdmsr_safe(msr, val) _rdmsr_safe(msr, val)
#endif  /* UXEN_HOST_OSX */

/* wrmsr with exception handling */
static inline int wrmsr_safe(unsigned int msr, uint64_t val)
{
    int _rc;
    uint32_t lo, hi;
    lo = (uint32_t)val;
    hi = (uint32_t)(val >> 32);

    __asm__ __volatile__(
        "1: wrmsr\n2:\n"
        ".section .fixup,\"ax\"\n"
        "3: movl %5,%0\n; jmp 2b\n"
        _ASM_PREVIOUS "\n"
        _ASM_EXTABLE(1b, 3b)
        : "=&r" (_rc)
        : "c" (msr), "a" (lo), "d" (hi), "0" (0), "i" (-EFAULT));
    return _rc;
}

#define rdtsc(low,high) \
     __asm__ __volatile__("rdtsc" : "=a" (low), "=d" (high))

#define rdtscl(low) \
     __asm__ __volatile__("rdtsc" : "=a" (low) : : "edx")

#if defined(__i386__)
#define rdtscll(val) \
     __asm__ __volatile__("rdtsc" : "=A" (val))
#elif defined(__x86_64__)
#define rdtscll(val) do { \
     unsigned int a,d; \
     asm volatile("rdtsc" : "=a" (a), "=d" (d)); \
     (val) = ((unsigned long)a) | (((unsigned long)d)<<32); \
} while(0)
#endif

#define __write_tsc(val) wrmsrl(MSR_IA32_TSC, val)
#define write_tsc(val) ({                                       \
    /* Reliable TSCs are in lockstep across all CPUs. We should \
     * never write to them. */                                  \
    ASSERT(!boot_cpu_has(X86_FEATURE_TSC_RELIABLE));            \
    __write_tsc(val);                                           \
})

#define write_rdtscp_aux(val) wrmsr(MSR_TSC_AUX, (val), 0)

#define rdpmc(counter,low,high) \
     __asm__ __volatile__("rdpmc" \
			  : "=a" (low), "=d" (high) \
			  : "c" (counter))


DECLARE_PER_CPU(u64, efer);
u64 read_efer(void);
void write_efer(u64 val);

DECLARE_PER_CPU(u32, ler_msr);

#endif /* !__ASSEMBLY__ */

#endif /* __ASM_MSR_H */
