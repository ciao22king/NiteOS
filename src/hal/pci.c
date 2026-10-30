#include <hal.h>
#include <string.h>
#include <console.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static inline u32 addr(u8 bus, u8 slot, u8 func, u8 offset)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)slot << 11) |
           ((u32)func << 8) | (offset & 0xFC);
}

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset)
{
    outl(PCI_CONFIG_ADDR, addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset)
{
    u32 v = pci_config_read32(bus, slot, func, offset);
    return (u16)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset)
{
    u32 v = pci_config_read32(bus, slot, func, offset);
    return (u8)((v >> ((offset & 3) * 8)) & 0xFF);
}

void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value)
{
    outl(PCI_CONFIG_ADDR, addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 value)
{
    u32 old = pci_config_read32(bus, slot, func, offset);
    int shift = (offset & 2) * 8;
    u32 merged = (old & ~(0xFFFFu << shift)) | ((u32)value << shift);
    pci_config_write32(bus, slot, func, offset, merged);
}

static void read_device(struct pci_device *dev)
{
    dev->vendor_id = pci_config_read16(dev->bus, dev->slot, dev->func, 0x00);
    dev->device_id = pci_config_read16(dev->bus, dev->slot, dev->func, 0x02);
    dev->revision = pci_config_read8(dev->bus, dev->slot, dev->func, 0x08);
    dev->prog_if = pci_config_read8(dev->bus, dev->slot, dev->func, 0x09);
    dev->subclass = pci_config_read8(dev->bus, dev->slot, dev->func, 0x0A);
    dev->class_code = pci_config_read8(dev->bus, dev->slot, dev->func, 0x0B);
    dev->header_type = pci_config_read8(dev->bus, dev->slot, dev->func, 0x0E);

    if ((dev->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++)
            dev->bar[i] = pci_config_read32(dev->bus, dev->slot, dev->func, 0x10 + i * 4);
        dev->irq_line = pci_config_read8(dev->bus, dev->slot, dev->func, 0x3C);
        dev->irq_pin = pci_config_read8(dev->bus, dev->slot, dev->func, 0x3D);
    } else {
        for (int i = 0; i < 6; i++) dev->bar[i] = 0;
        dev->irq_line = dev->irq_pin = 0;
    }

    dev->subsystem_vendor = pci_config_read16(dev->bus, dev->slot, dev->func, 0x2C);
    dev->subsystem_id = pci_config_read16(dev->bus, dev->slot, dev->func, 0x2E);
}

void pci_enumerate(pci_enum_cb cb, void *ctx)
{
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            u16 vendor = pci_config_read16((u8)bus, (u8)slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            u8 header = pci_config_read8((u8)bus, (u8)slot, 0, 0x0E);
            int funcs = (header & 0x80) ? 8 : 1;
            for (int func = 0; func < funcs; func++) {
                struct pci_device dev;
                memset(&dev, 0, sizeof(dev));
                dev.bus = (u8)bus;
                dev.slot = (u8)slot;
                dev.func = (u8)func;
                read_device(&dev);
                if (dev.vendor_id == 0xFFFF) continue;
                if (cb) cb(&dev, ctx);
            }
        }
    }
}

struct class_scan {
    u8 cls, sub;
    int want;
    int seen;
    struct pci_device *out;
};

static void class_scan_cb(const struct pci_device *d, void *c)
{
    struct class_scan *s = c;
    if (s->seen < 0) return;
    if (d->class_code != s->cls || d->subclass != s->sub) return;
    if (s->seen == s->want) {
        *s->out = *d;
        s->seen = -1;   /* signal found */
        return;
    }
    s->seen++;
}

int pci_find_class(u8 class_code, u8 subclass, struct pci_device *out, int index)
{
    struct class_scan s = { class_code, subclass, index, 0, out };
    pci_enumerate(class_scan_cb, &s);
    return s.seen == -1;
}

void pci_enable_bus_mastering(const struct pci_device *dev)
{
    u32 cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 2) | (1 << 1);   /* bus master | memory space */
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_enable_memory(const struct pci_device *dev)
{
    u32 cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= (1 << 1);
    pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

u32 pci_bar_size(const struct pci_device *dev, int bar_index)
{
    u8 off = (u8)(0x10 + bar_index * 4);
    u32 orig = pci_config_read32(dev->bus, dev->slot, dev->func, off);
    pci_config_write32(dev->bus, dev->slot, dev->func, off, 0xFFFFFFFF);
    u32 size_mask = pci_config_read32(dev->bus, dev->slot, dev->func, off);
    pci_config_write32(dev->bus, dev->slot, dev->func, off, orig);
    if (size_mask == 0) return 0;
    if (size_mask & 0x1) return 0;      /* I/O BAR */
    return ~(size_mask & ~0xF) + 1;
}

int pci_is_multifunction(const struct pci_device *dev)
{
    return (dev->header_type & 0x80) != 0;
}

struct name_pair { u16 id; const char *name; };

static const struct name_pair vendor_names[] = {
    { 0x8086, "Intel" },        { 0x1022, "AMD" },
    { 0x10DE, "NVIDIA" },       { 0x1002, "AMD/ATI" },
    { 0x10EC, "Realtek" },      { 0x14E4, "Broadcom" },
    { 0x1AF4, "Virtio" },       { 0x1B36, "Red Hat (QEMU)" },
    { 0x15AD, "VMware" },       { 0x1234, "Bochs/QEMU std VGA" },
    { 0x80EE, "VirtualBox" },   { 0x1D0F, "Amazon" },
    { 0x8087, "Intel (wireless)" }, { 0x1969, "Qualcomm Atheros" },
    { 0x1814, "Ralink" },       { 0x168C, "Qualcomm Atheros" },
    { 0x10DE, "NVIDIA" },       { 0x104C, "Texas Instruments" },
    { 0x1B21, "ASMedia" },      { 0x1D6B, "Linux Foundation" },
};

const char *pci_vendor_name(u16 vendor_id)
{
    for (usize i = 0; i < sizeof(vendor_names) / sizeof(vendor_names[0]); i++)
        if (vendor_names[i].id == vendor_id) return vendor_names[i].name;
    return NULL;
}

struct class_pair { u8 cls, sub; const char *name; };

static const struct class_pair class_names[] = {
    { 0x00, 0x00, "Unclassified" },
    { 0x01, 0x00, "SCSI storage controller" },
    { 0x01, 0x01, "IDE controller" },
    { 0x01, 0x06, "SATA controller" },
    { 0x01, 0x08, "NVMe controller" },
    { 0x01, 0x80, "Mass storage controller" },
    { 0x02, 0x00, "Ethernet controller" },
    { 0x02, 0x80, "Network controller" },
    { 0x03, 0x00, "VGA compatible controller" },
    { 0x03, 0x02, "3D controller" },
    { 0x04, 0x01, "Multimedia audio controller" },
    { 0x04, 0x03, "Audio device" },
    { 0x05, 0x00, "RAM controller" },
    { 0x06, 0x00, "Host bridge" },
    { 0x06, 0x01, "ISA bridge" },
    { 0x06, 0x04, "PCI-to-PCI bridge" },
    { 0x06, 0x80, "Bridge" },
    { 0x07, 0x00, "Serial controller" },
    { 0x07, 0x01, "Parallel controller" },
    { 0x08, 0x00, "Interrupt controller" },
    { 0x08, 0x01, "PIC" },
    { 0x08, 0x02, "DMA controller" },
    { 0x08, 0x03, "Timer" },
    { 0x0C, 0x03, "USB controller" },
    { 0x0C, 0x05, "SMBus controller" },
    { 0x0D, 0x00, "Wireless controller" },
    { 0x0D, 0x11, "Bluetooth controller" },
    { 0x10, 0x00, "Encryption controller" },
    { 0x11, 0x00, "Signal processing controller" },
};

const char *pci_class_name(u8 class_code, u8 subclass)
{
    for (usize i = 0; i < sizeof(class_names) / sizeof(class_names[0]); i++)
        if (class_names[i].cls == class_code && class_names[i].sub == subclass)
            return class_names[i].name;
    return "Unknown device class";
}
