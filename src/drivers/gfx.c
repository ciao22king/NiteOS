#include <gfx.h>
#include <hal.h>
#include <console.h>
#include <string.h>
#include <mm.h>
#include <mb2_parse.h>
#include <multiboot2.h>

/* Bochs/QEMU VBE "dispi" interface, exposed on the legacy VGA I/O ports. */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7

#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_LFB_ENABLED  0x40
#define VBE_DISPI_NOCLEARMEM   0x80

/* Where the framebuffer MMIO window is mapped in kernel virtual space. */
#define GFX_VIRT_BASE 0xFFFFFFFFE0000000ULL

static struct gfx_mode mode;

static void dispi_write(u16 index, u16 value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static u16 dispi_read(u16 index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static int bochs_set_mode(u32 w, u32 h, u32 bpp)
{
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, (u16)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (u16)h);
    dispi_write(VBE_DISPI_INDEX_BPP, (u16)bpp);
    dispi_write(VBE_DISPI_INDEX_VIRT_WIDTH, (u16)w);
    dispi_write(VBE_DISPI_INDEX_VIRT_HEIGHT, (u16)h);
    dispi_write(VBE_DISPI_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED | VBE_DISPI_NOCLEARMEM);

    /* Confirm the adapter accepted the request. */
    if (dispi_read(VBE_DISPI_INDEX_XRES) != (u16)w) return -1;
    if (dispi_read(VBE_DISPI_INDEX_YRES) != (u16)h) return -1;
    if (dispi_read(VBE_DISPI_INDEX_BPP) != (u16)bpp) return -1;
    return 0;
}

u32 gfx_rgb(u8 r, u8 g, u8 b)
{
    return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

void gfx_putpixel(u32 x, u32 y, u32 color)
{
    if (!mode.ready || x >= mode.width || y >= mode.height) return;
    u8 *p = (u8 *)mode.fb_virt + (u64)y * mode.pitch + (u64)x * mode.bytes_per_pixel;
    switch (mode.bytes_per_pixel) {
    case 4: *(volatile u32 *)p = color; break;
    case 3: p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF; break;
    case 2: *(volatile u16 *)p = (u16)color; break;
    default: *p = (u8)color; break;
    }
}

void gfx_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (!mode.ready) return;
    if (x + w > mode.width) w = mode.width - x;
    if (y + h > mode.height) h = mode.height - y;
    for (u32 row = 0; row < h; row++) {
        u8 *p = (u8 *)mode.fb_virt + (u64)(y + row) * mode.pitch + (u64)x * mode.bytes_per_pixel;
        if (mode.bytes_per_pixel == 4) {
            u32 *p32 = (u32 *)p;
            for (u32 col = 0; col < w; col++) p32[col] = color;
        } else {
            for (u32 col = 0; col < w; col++) {
                u8 *q = p + (u64)col * mode.bytes_per_pixel;
                switch (mode.bytes_per_pixel) {
                case 3: q[0] = color & 0xFF; q[1] = (color >> 8) & 0xFF; q[2] = (color >> 16) & 0xFF; break;
                case 2: *(u16 *)q = (u16)color; break;
                default: *q = (u8)color; break;
                }
            }
        }
    }
}

void gfx_draw_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_clear(u32 color)
{
    gfx_fill_rect(0, 0, mode.width, mode.height, color);
}

void gfx_scroll_up(u32 pixels, u32 fill_color)
{
    if (!mode.ready || pixels == 0 || pixels >= mode.height) return;
    u64 bytes = (u64)mode.pitch * (mode.height - pixels);
    memmove(mode.fb_virt, (u8 *)mode.fb_virt + (u64)mode.pitch * pixels, bytes);
    gfx_fill_rect(0, mode.height - pixels, mode.width, pixels, fill_color);
}

const struct gfx_mode *gfx_get_mode(void) { return &mode; }

/* If the bootloader already handed us a linear framebuffer, prefer it. */
static int use_bootloader_framebuffer(struct hw_profile *hw)
{
    if (!hw->has_framebuffer) return -1;
    mode.fb_phys = hw->fb_phys;
    mode.width = hw->fb_width;
    mode.height = hw->fb_height;
    mode.pitch = hw->fb_pitch;
    mode.bpp = hw->fb_bpp;
    mode.bytes_per_pixel = hw->fb_bpp / 8;
    strlcpy(mode.driver, "bootloader-framebuffer", sizeof(mode.driver));
    return 0;
}

int gfx_init(struct hw_profile *hw)
{
    memset(&mode, 0, sizeof(mode));

    if (use_bootloader_framebuffer(hw) != 0) {
        /* No bootloader framebuffer: program a mode on a known adapter. */
        struct pci_device vga;
        if (!device_find_class(hw, 0x03, 0x00, &vga, 0)) {
            klog_warn("no display adapter found on the PCI bus");
            return -1;
        }

        if (vga.vendor_id == 0x1234 && vga.device_id == 0x1111) {
            /* QEMU/Bochs standard VGA */
            if (bochs_set_mode(1024, 768, 32) != 0) {
                klog_err("Bochs VBE mode set failed");
                return -1;
            }
            mode.fb_phys = vga.bar[0] & 0xFFFFFFF0u;
            mode.width = 1024;
            mode.height = 768;
            mode.bpp = 32;
            mode.bytes_per_pixel = 4;
            mode.pitch = mode.width * mode.bytes_per_pixel;
            strlcpy(mode.driver, "bochs-vbe", sizeof(mode.driver));
        } else {
            klog_warn("unsupported display adapter %04x:%04x (no mode-setting driver)",
                      vga.vendor_id, vga.device_id);
            return -1;
        }
    }

    /* Map the framebuffer into kernel space; it usually sits above the
       1 GiB direct map, so an explicit mapping is required. */
    u64 fb_size = ALIGN_UP((u64)mode.pitch * mode.height, PAGE_SIZE);
    if (vmm_map_range(GFX_VIRT_BASE, mode.fb_phys, fb_size,
                      PAGE_WRITE | PAGE_NX) != 0) {
        klog_err("failed to map framebuffer (%u MiB) at %p",
                 (u32)(fb_size >> 20), (void *)mode.fb_phys);
        return -1;
    }
    mode.fb_virt = (void *)GFX_VIRT_BASE;
    mode.ready = 1;

    klog_ok("graphics: %s, %ux%u %u bpp, framebuffer %p -> %p",
            mode.driver, mode.width, mode.height, mode.bpp,
            (void *)mode.fb_phys, mode.fb_virt);

    console_attach_fb(mode.fb_virt, mode.width, mode.height, mode.pitch, mode.bpp);
    return 0;
}
