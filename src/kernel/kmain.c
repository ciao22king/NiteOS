#include <nitekernel.h>
#include <console.h>
#include <string.h>
#include <cpu.h>
#include <mm.h>
#include <multiboot2.h>
#include <mb2_parse.h>
#include <devices.h>
#include <gfx.h>
#include <ata.h>
#include <blockdev.h>
#include <neofs.h>
#include <net.h>
#include <dns.h>
#include <store.h>
#include <input.h>
#include <shell.h>
#include <syscall.h>
#include <process.h>
#include <lash.h>

/* Declared in the linker script. */
extern u8 kernel_end[];

static struct mb2_info *mb_info;
static int boot_complete;

static void mem_selftest(void)
{
    /* Physical allocator: allocate two pages, ensure they differ and are
       within the managed window, then free them. */
    u64 p1 = pmm_alloc_page();
    u64 p2 = pmm_alloc_page();
    if (!p1 || !p2 || p1 == p2 || !pmm_is_managed(p1) || !pmm_is_managed(p2))
        panic("pmm selftest failed (p1=%p p2=%p)", (void *)p1, (void *)p2);
    pmm_free_page(p1);
    pmm_free_page(p2);
    klog_ok("PMM: alloc/free cycle ok (p1=%p p2=%p)", (void *)p1, (void *)p2);

    /* Virtual mapping: map a fresh page at a scratch virtual address and
       confirm the translation round-trips. */
    u64 scratch = 0xFFFFFFFFD0000000ULL;
    u64 pp = pmm_alloc_page();
    if (vmm_map(scratch, pp, PAGE_WRITE) != 0) panic("vmm_map failed");
    volatile u64 *cell = (volatile u64 *)scratch;
    *cell = 0xDEADBEEFCAFEF00DULL;
    if (*cell != 0xDEADBEEFCAFEF00DULL) panic("vmm write/read mismatch");
    if (vmm_get_phys(scratch) != pp) panic("vmm_get_phys mismatch");
    klog_ok("VMM: mapped %p -> %p, readback verified", (void *)scratch, (void *)pp);

    /* Heap: allocate, fill, realloc, free. */
    char *buf = kmalloc(100);
    if (!buf) panic("kmalloc failed");
    memset(buf, 'A', 99);
    buf[99] = '\0';
    if (strlen(buf) != 99) panic("heap content mismatch");
    buf = krealloc(buf, 4096);
    if (!buf || strlen(buf) != 99) panic("krealloc lost data");
    kfree(buf);
    klog_ok("HEAP: kmalloc/krealloc/kfree ok");
}

void kmain(u32 magic, u32 info_addr)
{
    console_init();

    console_printf("\n");
    console_printf("==================================================\n");
    console_printf("  %s %s  -  built " __DATE__ " " __TIME__ "\n",
                   NITEKERNEL_NAME, NITEKERNEL_VERSION);
    console_printf("==================================================\n");

    if (magic != MB2_BOOTLOADER_MAGIC) {
        klog_err("bad Multiboot2 magic 0x%x (expected 0x%x)", magic, MB2_BOOTLOADER_MAGIC);
        for (;;) hlt();
    }
    klog_ok("Multiboot2 handoff verified (magic 0x%x)", magic);

    mb_info = (struct mb2_info *)phys_to_virt(info_addr);
    mb2_set_info(mb_info);
    klog_ok("Multiboot2 info at phys 0x%x, kernel_end phys 0x%x",
            info_addr, (u32)virt_to_phys((u64)kernel_end));

    console_printf("\n-- CPU tables --\n");
    gdt_init();
    klog_ok("GDT + TSS loaded");
    pic_init();
    klog_ok("8259 PIC remapped to vectors 32..47");
    idt_init();
    klog_ok("IDT installed (48 gates), PIT at %u Hz", pit_get_frequency());
    interrupts_enable();
    klog_ok("interrupts enabled");

    console_printf("\n-- Memory management --\n");
    struct mb2_tag_mmap *mmap = (struct mb2_tag_mmap *)mb2_find(mb_info, MB2_TAG_MMAP);
    if (!mmap) panic("no memory map from bootloader");
    pmm_init(mmap, virt_to_phys((u64)kernel_end), 0);
    /* Reserve the physical span the bootloader used for the info structure. */
    pmm_reserve(info_addr, mb_info->total_size);
    klog_ok("PMM: %u MiB total, %u MiB free (%u pages in use)",
            (u32)(pmm_total_bytes() >> 20), (u32)(pmm_free_bytes() >> 20),
            (u32)(pmm_used_bytes() / PAGE_SIZE));
    vmm_init();
    heap_init();
    klog_ok("VMM + heap online");
    mem_selftest();

    console_printf("\n-- Hardware discovery --\n");
    device_discover(device_profile());

    console_printf("\n-- Graphics --\n");
    struct hw_profile *hw = device_profile();
    if (gfx_init(hw) == 0) {
        const struct gfx_mode *m = gfx_get_mode();
        hw->has_framebuffer = 1;
        hw->fb_phys = m->fb_phys;
        hw->fb_width = m->width;
        hw->fb_height = m->height;
        hw->fb_bpp = m->bpp;
        hw->fb_pitch = m->pitch;
    } else {
        klog_warn("running without a graphical framebuffer");
    }

    device_print_report(hw);

    console_printf("\n-- Storage --\n");
    ata_init();
    ata_print_devices();
    blockdev_init();
    blockdev_print();

    console_printf("\n-- Filesystem --\n");
    const struct block_device *disk = blockdev_first();
    if (disk) {
        if (neofs_mount(disk) != 0) {
            klog_warn("no NEOFS signature on %s; formatting (first boot)", disk->name);
            if (neofs_format(disk) == 0)
                klog_ok("NEOFS formatted on %s (%u sectors)", disk->name, disk->sector_count);
            else
                klog_err("could not format %s", disk->name);
        } else {
            klog_ok("NEOFS mounted on %s (%d file(s))", disk->name, neofs_file_count());
        }
        if (neofs_mounted()) {
            driverdb_load();
            klog_ok("driver database: %d record(s) on disk", driverdb_count());
        }
    } else {
        klog_warn("no ATA hard disk found; running without persistent storage");
    }

    console_printf("\n-- Networking --\n");
    if (net_probe() == 0 && net_init() == 0) {
        if (dhcp_configure(6000) == 0) {
            char ip[16], gw[16], dns[16];
            struct netif *n = net_iface();
            klog_ok("DHCP lease: ip %s gateway %s dns %s",
                    ip_str(n->ip, ip), ip_str(n->gateway, gw), ip_str(n->dns, dns));
        } else {
            klog_warn("DHCP failed; falling back to a static address");
            struct netif *n = net_iface();
            n->ip = ip_make(10, 0, 2, 15);
            n->netmask = ip_make(255, 255, 255, 0);
            n->gateway = ip_make(10, 0, 2, 2);
            n->dns = ip_make(10, 0, 2, 3);
            n->configured = 1;
        }
        net_print_status();

        console_printf("\n-- Driver acquisition --\n");
        klog_info("STORE", "checking %u store entr(ies) against the hardware profile",
                  store_entry_count());
        int installed = store_acquire(hw);
        klog_ok("driver acquisition done: %d new driver(s), %d in database",
                installed, driverdb_count());
        driverdb_print();
    } else {
        klog_warn("no supported NIC found");
    }

    console_printf("\n-- Executable formats --\n");
    syscall_init();
    process_init();
    lash_init();
    klog_ok("ring-3 loader ready: ELF64 (.appimage), PE32+ (.exe), .lash native");
    klog_ok("syscall gate int 0x80 installed (DPL 3)");

    console_printf("\n-- Input --\n");
    shell_init();
    klog_ok("keyboard online (PS/2, IRQ 1)");

    console_printf("\n%s %s is up. Serial console is interactive.\n",
                   NITEKERNEL_NAME, NITEKERNEL_VERSION);
    console_printf("Type 'help' for the command list.\n");
    boot_complete = 1;

    shell_run();
}
