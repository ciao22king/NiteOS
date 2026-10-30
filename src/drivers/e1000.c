#include <net.h>
#include <hal.h>
#include <console.h>
#include <string.h>
#include <mm.h>

/* Intel 82540EM (e1000) driver, polled RX/TX. */

#define E1000_VENDOR 0x8086

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EECD    0x0010
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_RAL     0x5400
#define REG_RAH     0x5404

#define CTRL_RST   (1u << 26)
#define CTRL_SLU   (1u << 6)
#define STATUS_LU  (1u << 1)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define RCTL_BSIZE_2048 (0u << 16)

#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)

#define RX_DESC_COUNT 32
#define TX_DESC_COUNT 8
#define BUF_SIZE      2048

#define MMIO_VIRT 0xFFFFFFFFE1000000ULL

struct e1000_rx_desc {
    u64 addr;
    u16 length;
    u16 checksum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed));

struct e1000_tx_desc {
    u64 addr;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed));

#define RXD_STAT_DD  0x01
#define RXD_STAT_EOP 0x02
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STAT_DD  0x01

static volatile u8 *regs;
static struct e1000_rx_desc *rx_ring;
static struct e1000_tx_desc *tx_ring;
static u64 rx_ring_phys, tx_ring_phys;
static u64 rx_buf_phys, tx_buf_phys;
static u32 rx_cur, tx_cur;
static u8 mac[ETH_ALEN];
static int initialized;

static inline u32 mmio_read(u32 reg) { return *(volatile u32 *)(regs + reg); }
static inline void mmio_write(u32 reg, u32 val) { *(volatile u32 *)(regs + reg) = val; }

static u16 eeprom_read(u8 addr)
{
    mmio_write(REG_EERD, ((u32)addr << 8) | 1);
    u32 val;
    u32 guard = 1000000;
    do {
        val = mmio_read(REG_EERD);
    } while (!(val & (1u << 4)) && --guard);
    return (u16)(val >> 16);
}

static void read_mac(void)
{
    u16 w0 = eeprom_read(0), w1 = eeprom_read(1), w2 = eeprom_read(2);
    if (w0 == 0 || w0 == 0xFFFF) {
        /* Fall back to the receive-address registers. */
        u32 lo = mmio_read(REG_RAL), hi = mmio_read(REG_RAH);
        mac[0] = lo & 0xFF; mac[1] = (lo >> 8) & 0xFF;
        mac[2] = (lo >> 16) & 0xFF; mac[3] = (lo >> 24) & 0xFF;
        mac[4] = hi & 0xFF; mac[5] = (hi >> 8) & 0xFF;
        return;
    }
    mac[0] = w0 & 0xFF; mac[1] = (w0 >> 8) & 0xFF;
    mac[2] = w1 & 0xFF; mac[3] = (w1 >> 8) & 0xFF;
    mac[4] = w2 & 0xFF; mac[5] = (w2 >> 8) & 0xFF;
}

static int e1000_send(const void *frame, u32 len)
{
    if (!initialized || len > BUF_SIZE) return -1;

    struct e1000_tx_desc *d = &tx_ring[tx_cur];
    u8 *dst = (u8 *)phys_to_virt(tx_buf_phys + (u64)tx_cur * BUF_SIZE);

    memset(dst, 0, len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len);
    memcpy(dst, frame, len);
    u32 wire_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;

    d->addr = tx_buf_phys + (u64)tx_cur * BUF_SIZE;
    d->length = (u16)wire_len;
    d->cso = 0;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;

    tx_cur = (tx_cur + 1) % TX_DESC_COUNT;
    mmio_write(REG_TDT, tx_cur);

    u32 guard = 1000000;
    while (!(tx_ring[(tx_cur + TX_DESC_COUNT - 1) % TX_DESC_COUNT].status & TXD_STAT_DD))
        if (!--guard) return -1;
    return (int)len;
}

static int e1000_recv(void *frame, u32 max)
{
    if (!initialized) return -1;
    struct e1000_rx_desc *d = &rx_ring[rx_cur];
    if (!(d->status & RXD_STAT_DD)) return 0;

    u32 len = d->length;
    if (len > max) len = max;
    memcpy(frame, (const void *)phys_to_virt(d->addr), len);

    d->status = 0;
    mmio_write(REG_RDT, rx_cur);
    rx_cur = (rx_cur + 1) % RX_DESC_COUNT;
    return (int)len;
}

static void e1000_get_mac(u8 *out) { memcpy(out, mac, ETH_ALEN); }

static int e1000_link_up(void)
{
    if (!initialized) return 0;
    return (mmio_read(REG_STATUS) & STATUS_LU) != 0;
}

static int e1000_probe(void)
{
    struct pci_device dev;
    if (!pci_find_class(0x02, 0x00, &dev, 0)) return 0;
    return dev.vendor_id == E1000_VENDOR;
}

int e1000_init(void)
{
    struct pci_device dev;
    if (!pci_find_class(0x02, 0x00, &dev, 0)) return -1;
    if (dev.vendor_id != E1000_VENDOR) return -1;

    /* BAR0 is the MMIO window for this controller. */
    u32 bar0 = dev.bar[0] & 0xFFFFFFF0u;
    if (bar0 == 0) return -1;

    pci_enable_memory(&dev);
    pci_enable_bus_mastering(&dev);

    u64 mmio_size = ALIGN_UP(pci_bar_size(&dev, 0), PAGE_SIZE);
    if (mmio_size == 0 || mmio_size > (1ULL << 20)) mmio_size = 128 * 1024;
    if (vmm_map_range(MMIO_VIRT, bar0, mmio_size, PAGE_WRITE | PAGE_NX) != 0)
        return -1;
    regs = (volatile u8 *)MMIO_VIRT;

    /* Reset the controller. */
    mmio_write(REG_IMC, 0xFFFFFFFF);
    u32 ctrl = mmio_read(REG_CTRL);
    mmio_write(REG_CTRL, ctrl | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++) cpu_relax();
    mmio_write(REG_IMC, 0xFFFFFFFF);   /* keep interrupts masked: RX is polled */

    /* Descriptor rings and DMA buffers (physical memory, direct-mapped). */
    rx_ring_phys = pmm_alloc_pages(1);
    tx_ring_phys = pmm_alloc_pages(1);
    rx_buf_phys = pmm_alloc_pages((RX_DESC_COUNT * BUF_SIZE) / PAGE_SIZE);
    tx_buf_phys = pmm_alloc_pages((TX_DESC_COUNT * BUF_SIZE) / PAGE_SIZE);
    if (!rx_ring_phys || !tx_ring_phys || !rx_buf_phys || !tx_buf_phys)
        return -1;

    rx_ring = (struct e1000_rx_desc *)phys_to_virt(rx_ring_phys);
    tx_ring = (struct e1000_tx_desc *)phys_to_virt(tx_ring_phys);
    memset(rx_ring, 0, PAGE_SIZE);
    memset(tx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < RX_DESC_COUNT; i++) {
        rx_ring[i].addr = rx_buf_phys + (u64)i * BUF_SIZE;
        rx_ring[i].status = 0;
    }
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_ring[i].addr = tx_buf_phys + (u64)i * BUF_SIZE;
        tx_ring[i].status = TXD_STAT_DD;
    }

    mmio_write(REG_RDBAL, (u32)(rx_ring_phys & 0xFFFFFFFF));
    mmio_write(REG_RDBAH, (u32)(rx_ring_phys >> 32));
    mmio_write(REG_RDLEN, RX_DESC_COUNT * sizeof(struct e1000_rx_desc));
    mmio_write(REG_RDH, 0);
    mmio_write(REG_RDT, RX_DESC_COUNT - 1);
    mmio_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    mmio_write(REG_TDBAL, (u32)(tx_ring_phys & 0xFFFFFFFF));
    mmio_write(REG_TDBAH, (u32)(tx_ring_phys >> 32));
    mmio_write(REG_TDLEN, TX_DESC_COUNT * sizeof(struct e1000_tx_desc));
    mmio_write(REG_TDH, 0);
    mmio_write(REG_TDT, 0);
    mmio_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));

    read_mac();
    mmio_write(REG_RAL, mac[0] | (mac[1] << 8) | (mac[2] << 16) | ((u32)mac[3] << 24));
    mmio_write(REG_RAH, mac[4] | (mac[5] << 8) | (1u << 31));

    mmio_write(REG_CTRL, mmio_read(REG_CTRL) | CTRL_SLU);
    mmio_read(REG_STATUS);   /* flush */

    rx_cur = 0;
    tx_cur = 0;
    initialized = 1;

#ifdef E1000_DEBUG
    klog_info("E1000", "bar0=0x%x size=%u rx_ring=%p tx_ring=%p",
              bar0, (u32)mmio_size, (void *)rx_ring_phys, (void *)tx_ring_phys);
    klog_info("E1000", "CTRL=0x%08x STATUS=0x%08x RCTL=0x%08x TCTL=0x%08x",
              mmio_read(REG_CTRL), mmio_read(REG_STATUS),
              mmio_read(REG_RCTL), mmio_read(REG_TCTL));
    klog_info("E1000", "RDBAL=0x%08x RDBAH=0x%08x RDLEN=%u RDH=%u RDT=%u",
              mmio_read(REG_RDBAL), mmio_read(REG_RDBAH), mmio_read(REG_RDLEN),
              mmio_read(REG_RDH), mmio_read(REG_RDT));
    klog_info("E1000", "TDBAL=0x%08x TDLEN=%u TDH=%u TDT=%u",
              mmio_read(REG_TDBAL), mmio_read(REG_TDLEN),
              mmio_read(REG_TDH), mmio_read(REG_TDT));
#endif
    return 0;
}

static const struct nic_ops e1000_ops = {
    .name = "e1000",
    .probe = e1000_probe,
    .init = e1000_init,
    .send = e1000_send,
    .recv = e1000_recv,
    .get_mac = e1000_get_mac,
    .link_up = e1000_link_up,
};

const struct nic_ops *e1000_driver(void) { return &e1000_ops; }
