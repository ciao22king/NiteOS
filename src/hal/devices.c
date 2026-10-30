#include <devices.h>
#include <console.h>
#include <string.h>
#include <mm.h>
#include <cpu.h>
#include <multiboot2.h>
#include <mb2_parse.h>

extern struct mb2_info *neo_mb_info(void);
extern void rtc_read(struct hw_profile *p);

struct pci_ctx {
    struct hw_profile *p;
    u32 count;
};

static void collect_pci(const struct pci_device *dev, void *ctx)
{
    struct pci_ctx *c = ctx;
    if (c->p->pci_count < HW_MAX_PCI)
        c->p->pci[c->p->pci_count++] = *dev;
    c->count++;
}

/* The kernel keeps a single hardware profile; the shell and the driver
   store both read it. */
static struct hw_profile g_profile;

struct hw_profile *device_profile(void) { return &g_profile; }

void device_discover(struct hw_profile *p)
{
    memset(p, 0, sizeof(*p));

    struct cpu_info cpu;
    cpuid_detect(&cpu);
    strlcpy(p->cpu_brand, cpu.brand, sizeof(p->cpu_brand));
    strlcpy(p->cpu_vendor, cpu.vendor, sizeof(p->cpu_vendor));
    p->cpu_cores = cpu.physical_cores;
    p->cpu_logical = cpu.logical_cores;
    p->cpu_family = cpu.family;
    p->cpu_model = cpu.model;
    if (cpu.has_apic)   p->cpu_flags |= CPUF_APIC;
    if (cpu.has_x2apic) p->cpu_flags |= CPUF_X2APIC;
    if (cpu.has_sse2)   p->cpu_flags |= CPUF_SSE2;
    if (cpu.has_avx)    p->cpu_flags |= CPUF_AVX;
    if (cpu.has_avx2)   p->cpu_flags |= CPUF_AVX2;
    if (cpu.has_nx)     p->cpu_flags |= CPUF_NX;
    if (cpu.has_vmx)    p->cpu_flags |= CPUF_VMX;
    if (cpu.has_svm)    p->cpu_flags |= CPUF_SVM;

    p->mem_total = pmm_total_bytes();
    p->mem_free = pmm_free_bytes();

    /* Framebuffer from the bootloader, if it provided one. */
    struct mb2_info *info = neo_mb_info();
    struct mb2_tag_framebuffer *fb =
        (struct mb2_tag_framebuffer *)mb2_find(info, MB2_TAG_FRAMEBUFFER);
    if (fb && fb->framebuffer_type == MB2_FB_TYPE_RGB && fb->framebuffer_addr) {
        p->has_framebuffer = 1;
        p->fb_phys = fb->framebuffer_addr;
        p->fb_width = fb->framebuffer_width;
        p->fb_height = fb->framebuffer_height;
        p->fb_bpp = fb->framebuffer_bpp;
        p->fb_pitch = fb->framebuffer_pitch;
    }

    struct pci_ctx ctx = { p, 0 };
    pci_enumerate(collect_pci, &ctx);

    rtc_read(p);
}

int device_count_class(const struct hw_profile *p, u8 class_code, u8 subclass)
{
    int n = 0;
    for (u32 i = 0; i < p->pci_count; i++)
        if (p->pci[i].class_code == class_code && p->pci[i].subclass == subclass) n++;
    return n;
}

int device_find_class(const struct hw_profile *p, u8 class_code, u8 subclass,
                      struct pci_device *out, int index)
{
    int seen = 0;
    for (u32 i = 0; i < p->pci_count; i++) {
        if (p->pci[i].class_code != class_code || p->pci[i].subclass != subclass)
            continue;
        if (seen++ == index) {
            if (out) *out = p->pci[i];
            return 1;
        }
    }
    return 0;
}

static void print_pci_line(const struct pci_device *d)
{
    const char *vname = pci_vendor_name(d->vendor_id);
    char vid[16];
    if (!vname) { snprintf(vid, sizeof(vid), "%04x", d->vendor_id); vname = vid; }

    klog_info("PCI", "%02x:%02x.%x  %-16s [%04x:%04x]  %s%s",
              d->bus, d->slot, d->func, vname, d->vendor_id, d->device_id,
              pci_class_name(d->class_code, d->subclass),
              pci_is_multifunction(d) ? " (multifunction)" : "");
}

void device_print_report(const struct hw_profile *p)
{
    console_printf("\n================ PC COMPONENT REPORT ================\n");

    klog_info("CPU", "%-16s %s", "Processor:", p->cpu_brand);
    klog_info("CPU", "%-16s %s (family %u model %u)", "Vendor:", p->cpu_vendor,
              p->cpu_family, p->cpu_model);
    klog_info("CPU", "%-16s %u cores / %u threads", "Topology:",
              p->cpu_cores, p->cpu_logical);

    klog_info("MEM", "%-16s %u MiB installed, %u MiB free",
              "System memory:", (u32)(p->mem_total >> 20), (u32)(p->mem_free >> 20));

    if (p->has_framebuffer)
        klog_info("GPU", "%-16s %ux%u @ %u bpp (linear framebuffer @ %p)",
                  "Display:", p->fb_width, p->fb_height, p->fb_bpp,
                  (void *)p->fb_phys);
    else
        klog_warn("no linear framebuffer reported by the bootloader (VGA text only)");

    int gpus = device_count_class(p, 0x03, 0x00) + device_count_class(p, 0x03, 0x02);
    int nics = device_count_class(p, 0x02, 0x00);
    int storage = device_count_class(p, 0x01, 0x06) + device_count_class(p, 0x01, 0x01) +
                  device_count_class(p, 0x01, 0x08);
    int audio = device_count_class(p, 0x04, 0x01) + device_count_class(p, 0x04, 0x03);
    int usb = device_count_class(p, 0x0C, 0x03);

    klog_info("BUS", "%-16s %u devices enumerated", "PCI bus:", p->pci_count);
    klog_info("SUM", "%-16s %d GPU, %d NIC, %d storage, %d audio, %d USB",
              "Subsystems:", gpus, nics, storage, audio, usb);

    console_printf("-----------------------------------------------------\n");
    for (u32 i = 0; i < p->pci_count; i++)
        print_pci_line(&p->pci[i]);

    if (p->has_rtc)
        klog_info("RTC", "%-16s %04d-%02d-%02d %02d:%02d:%02d UTC", "Hardware clock:",
                  p->rtc_year, p->rtc_month, p->rtc_day,
                  p->rtc_hour, p->rtc_minute, p->rtc_second);

    console_printf("=====================================================\n");
}
