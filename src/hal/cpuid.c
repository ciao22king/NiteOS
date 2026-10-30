#include <hal.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

static inline void cpuid_leaf(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static u64 g_tsc_hz;

void cpuid_detect(struct cpu_info *info)
{
    memset(info, 0, sizeof(*info));

    u32 a, b, c, d;
    cpuid_leaf(0, 0, &a, &b, &c, &d);
    info->max_leaf = a;
    memcpy(info->vendor + 0, &b, 4);
    memcpy(info->vendor + 4, &d, 4);
    memcpy(info->vendor + 8, &c, 4);
    info->vendor[12] = '\0';

    cpuid_leaf(1, 0, &a, &b, &c, &d);
    info->stepping = a & 0xF;
    info->model = (a >> 4) & 0xF;
    info->family = (a >> 8) & 0xF;
    if (info->family == 0xF) info->family += (a >> 20) & 0xFF;
    if (info->family == 0x6 || info->family == 0xF) info->model += ((a >> 16) & 0xF) << 4;
    info->logical_cores = (b >> 16) & 0xFF;
    if (info->logical_cores == 0) info->logical_cores = 1;
    info->apic_id = (b >> 24) & 0xFF;

    info->has_msr    = (d >> 5) & 1;
    info->has_apic   = (d >> 9) & 1;
    info->has_sse    = (d >> 25) & 1;
    info->has_sse2   = (d >> 26) & 1;
    info->has_vmx    = (c >> 5) & 1;
    info->has_x2apic = (c >> 21) & 1;
    info->has_avx    = (c >> 28) & 1;
    info->has_fsgsbase = (b >> 0) & 1;
    info->has_pcid   = (c >> 17) & 1;
    info->hypervisor = (c >> 31) & 1;

    /* NX is reported via the extended feature bits. */
    cpuid_leaf(0x80000000, 0, &a, &b, &c, &d);
    info->max_ext_leaf = a;
    if (info->max_ext_leaf >= 0x80000001) {
        cpuid_leaf(0x80000001, 0, &a, &b, &c, &d);
        info->has_nx  = (d >> 20) & 1;
        info->has_svm = (c >> 2) & 1;
    }

    /* Extended brand string (leaves 0x80000002..0x80000004). */
    if (info->max_ext_leaf >= 0x80000004) {
        u32 *p = (u32 *)info->brand;
        for (u32 leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid_leaf(leaf, 0, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        info->brand[48] = '\0';
        str_trim(info->brand);
    }
    if (info->brand[0] == '\0')
        strlcpy(info->brand, "unknown x86-64 processor", sizeof(info->brand));

    /* AVX2 lives in leaf 7 subleaf 0. */
    if (info->max_leaf >= 7) {
        cpuid_leaf(7, 0, &a, &b, &c, &d);
        info->has_avx2 = (b >> 5) & 1;
    }

    /* Physical cores: leaf 0x0B level-2 gives threads per core. */
    info->physical_cores = info->logical_cores;
    if (info->max_leaf >= 0x0B) {
        cpuid_leaf(0x0B, 0, &a, &b, &c, &d);
        u32 threads_per_core = b & 0xFFFF;
        if (threads_per_core)
            info->physical_cores = info->logical_cores / threads_per_core;
        if (info->physical_cores == 0) info->physical_cores = 1;
    }
}

void cpuid_print(const struct cpu_info *info)
{
    klog_info("CPU", "%s", info->brand);
    klog_info("CPU", "vendor=%s family=%u model=%u stepping=%u",
              info->vendor, info->family, info->model, info->stepping);
    klog_info("CPU", "%u physical / %u logical cores, APIC id %u%s",
              info->physical_cores, info->logical_cores, info->apic_id,
              info->hypervisor ? " (virtualised)" : "");

    char feats[256] = {0};
#define ADD_FEAT(cond, name) if (cond) { if (feats[0]) strcat(feats, " "); strcat(feats, name); }
    ADD_FEAT(info->has_msr, "MSR");
    ADD_FEAT(info->has_apic, "APIC");
    ADD_FEAT(info->has_x2apic, "x2APIC");
    ADD_FEAT(info->has_sse, "SSE");
    ADD_FEAT(info->has_sse2, "SSE2");
    ADD_FEAT(info->has_avx, "AVX");
    ADD_FEAT(info->has_avx2, "AVX2");
    ADD_FEAT(info->has_nx, "NX");
    ADD_FEAT(info->has_pcid, "PCID");
    ADD_FEAT(info->has_fsgsbase, "FSGSBASE");
    ADD_FEAT(info->has_vmx, "VMX");
    ADD_FEAT(info->has_svm, "SVM");
#undef ADD_FEAT
    klog_info("CPU", "features: %s", feats);
}

u64 cpu_tsc_hz(void) { return g_tsc_hz; }
