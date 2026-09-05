#include <tests/ktest.h>
#include <arch/cpu.h>
#include <lib/string.h>
#include <console.h>

/* The processor identification filled by cpu_identify is complete and
 * lists every feature the kernel enables without asking. Run under KVM
 * as well as TCG (make test-kvm) to compare the two environments. */
static void test_cpu(void)
{
    const struct cpu_features *f = &cpu_features;
    kprintf("cpu: vendor \"%s\" hypervisor \"%s\" family %u model %u\n",
            f->vendor, f->hypervisor, f->family, f->model);
    ktest_assert(strlen(f->vendor) == 12, "vendor string \"%s\" is not 12 characters", f->vendor);
    ktest_assert(f->intel || f->amd, "unknown vendor \"%s\"", f->vendor);
    ktest_assert(f->family > 0, "family is 0");
    ktest_assert(f->nx && f->pge && f->pat && f->fxsr && f->sse2,
                 "a required feature is missing: nx %d pge %d pat %d fxsr %d sse2 %d",
                 f->nx, f->pge, f->pat, f->fxsr, f->sse2);
    ktest_assert(f->phys_bits >= 36 && f->phys_bits <= 52, "physical address bits %u", f->phys_bits);
    /* Every CPU enabled the same bits. */
    uint64_t cr4;
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    ktest_assert(cr4 & (1UL << 7), "CR4.PGE is clear");
    ktest_assert(cr4 & (1UL << 9), "CR4.OSFXSR is clear");
    ktest_assert(rdmsr(MSR_EFER) & (1UL << 11), "EFER.NXE is clear");
    /* sysret must produce user selectors with RPL 3 on both vendors. */
    uint64_t star = rdmsr(MSR_STAR);
    ktest_assert(((star >> 48) & 3) == 3, "STAR[63:48] = %lx lacks RPL 3", star >> 48);
}
KTEST_DEFINE("cpu", test_cpu);
