#ifndef NEO_HAL_H
#define NEO_HAL_H

#include <nitekernel.h>

/* ---------------- CPU ---------------- */
struct cpu_info {
    char vendor[16];
    char brand[52];
    u32 max_leaf;
    u32 max_ext_leaf;
    u32 family, model, stepping;
    u32 logical_cores;
    u32 physical_cores;
    u32 apic_id;
    int hypervisor;
    int has_apic, has_x2apic, has_msr, has_sse, has_sse2, has_avx, has_avx2;
    int has_vmx, has_svm, has_pcid, has_fsgsbase, has_nx;
    u32 tsc_hz_estimate;
};

void cpuid_detect(struct cpu_info *info);
void cpuid_print(const struct cpu_info *info);
u64  cpu_tsc_hz(void);

/* ---------------- PCI ---------------- */
struct pci_device {
    u8  bus, slot, func;
    u16 vendor_id, device_id;
    u16 subsystem_vendor, subsystem_id;
    u8  class_code, subclass, prog_if, revision;
    u8  header_type;
    u32 bar[6];
    u8  irq_line, irq_pin;
};

typedef void (*pci_enum_cb)(const struct pci_device *dev, void *ctx);

u32  pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16  pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
u8   pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset);
void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value);
void pci_enumerate(pci_enum_cb cb, void *ctx);
int  pci_find_class(u8 class_code, u8 subclass, struct pci_device *out, int index);
void pci_enable_bus_mastering(const struct pci_device *dev);
void pci_enable_memory(const struct pci_device *dev);
u32  pci_bar_size(const struct pci_device *dev, int bar_index);

const char *pci_vendor_name(u16 vendor_id);
const char *pci_class_name(u8 class_code, u8 subclass);
int  pci_is_multifunction(const struct pci_device *dev);

#endif
