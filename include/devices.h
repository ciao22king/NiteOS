#ifndef NEO_DEVICES_H
#define NEO_DEVICES_H

#include <nitekernel.h>
#include <hal.h>

#define HW_MAX_PCI 96

struct hw_profile {
    /* CPU */
    char cpu_brand[52];
    char cpu_vendor[16];
    u32  cpu_cores;
    u32  cpu_logical;
    u32  cpu_family, cpu_model;
    u32  cpu_flags;

    /* Memory */
    u64 mem_total;
    u64 mem_free;

    /* Display */
    int  has_framebuffer;
    u64  fb_phys;
    u32  fb_width, fb_height, fb_bpp, fb_pitch;

    /* Bus */
    u32  pci_count;
    struct pci_device pci[HW_MAX_PCI];

    /* Real-time clock (filled by the RTC driver when present) */
    int  has_rtc;
    int  rtc_year, rtc_month, rtc_day, rtc_hour, rtc_minute, rtc_second;

    /* Boot bookkeeping (filled by the profile store) */
    u32  boot_count;
};

/* CPU feature bits kept in hw_profile.cpu_flags */
#define CPUF_APIC   (1u << 0)
#define CPUF_X2APIC (1u << 1)
#define CPUF_SSE2   (1u << 2)
#define CPUF_AVX    (1u << 3)
#define CPUF_AVX2   (1u << 4)
#define CPUF_NX     (1u << 5)
#define CPUF_VMX    (1u << 6)
#define CPUF_SVM    (1u << 7)

void device_discover(struct hw_profile *p);
void device_print_report(const struct hw_profile *p);

/* The single hardware profile the kernel populates at boot. */
struct hw_profile *device_profile(void);

/* Counters used by the report. */
int  device_count_class(const struct hw_profile *p, u8 class_code, u8 subclass);
int  device_find_class(const struct hw_profile *p, u8 class_code, u8 subclass,
                       struct pci_device *out, int index);

#endif
