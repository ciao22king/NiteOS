#include <console.h>
#include <design.h>
#include <string.h>
#include <font8x16.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_MEM  ((volatile u16 *)phys_to_virt(0xB8000))

#define SERIAL_COM1 0x3F8

/* VGA text mode state */
static u8  vga_attr = VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
static int cursor_row = 0;
static int cursor_col = 0;

/* Framebuffer state — impeccable color system */
static u8  *fb_addr = NULL;
static u32  fb_width, fb_height, fb_pitch, fb_bpp;
static u32  fb_cx, fb_cy;
static u32  fb_fg = FB_FG, fb_bg = FB_BG;
static u32  fb_current_fg = FB_FG;  /* Current drawing color for styled output */

/* Serial COM1 */
static void serial_init(void)
{
    outb(SERIAL_COM1 + 1, 0x00);
    outb(SERIAL_COM1 + 3, 0x80);
    outb(SERIAL_COM1 + 0, 0x01);
    outb(SERIAL_COM1 + 1, 0x00);
    outb(SERIAL_COM1 + 3, 0x03);
    outb(SERIAL_COM1 + 2, 0xC7);
    outb(SERIAL_COM1 + 4, 0x0B);
}

static int serial_tx_ready(void)
{
    return inb(SERIAL_COM1 + 5) & 0x20;
}

static void serial_putc(char c)
{
    for (int spin = 0; spin < 100000 && !serial_tx_ready(); spin++)
        cpu_relax();
    outb(SERIAL_COM1, (u8)c);
}

static int serial_rx_ready(void)
{
    return inb(SERIAL_COM1 + 5) & 0x01;
}

int console_getchar(void)
{
    if (!serial_rx_ready()) return -1;
    return (u8)inb(SERIAL_COM1);
}

int console_has_input(void) { return serial_rx_ready(); }

/* VGA text mode */
static void vga_scroll(void)
{
    if (cursor_row < VGA_ROWS) return;
    for (int r = 1; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++)
            VGA_MEM[(r - 1) * VGA_COLS + c] = VGA_MEM[r * VGA_COLS + c];
    for (int c = 0; c < VGA_COLS; c++)
        VGA_MEM[(VGA_ROWS - 1) * VGA_COLS + c] = (u16)(vga_attr << 8) | ' ';
    cursor_row = VGA_ROWS - 1;
}

static void vga_putc(char c)
{
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\t') {
        cursor_col = (cursor_col + 4) & ~3;
    } else if (c == '\b') {
        if (cursor_col > 0) cursor_col--;
        VGA_MEM[cursor_row * VGA_COLS + cursor_col] = (u16)(vga_attr << 8) | ' ';
    } else {
        VGA_MEM[cursor_row * VGA_COLS + cursor_col] = (u16)(vga_attr << 8) | (u8)c;
        cursor_col++;
        if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; }
    }
    vga_scroll();
}

/* Framebuffer rendering — improved with impeccable typeset principles */
static void fb_putpixel(u32 x, u32 y, u32 color)
{
    if (x >= fb_width || y >= fb_height) return;
    u32 bytes = fb_bpp / 8;
    u8 *p = fb_addr + (u64)y * fb_pitch + (u64)x * bytes;
    if (bytes == 4) {
        *(u32 *)p = color;
    } else if (bytes == 3) {
        p[0] = color & 0xFF; p[1] = (color >> 8) & 0xFF; p[2] = (color >> 16) & 0xFF;
    } else if (bytes == 2) {
        *(u16 *)p = (u16)color;
    } else {
        *p = (u8)color;
    }
}

/* Draw a single glyph with current foreground color */
static void fb_draw_glyph(const u8 *glyph, u32 x, u32 y, u32 fg, u32 bg)
{
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            u32 color = (bits & (0x80 >> col)) ? fg : bg;
            fb_putpixel(x + col, y + row, color);
        }
    }
}

static void fb_putc(char c)
{
    if (!fb_addr) return;
    if (c == '\n') { fb_cx = 0; fb_cy += 16; return; }
    if (c == '\r') { fb_cx = 0; return; }
    if (c == '\b') { if (fb_cx >= 8) fb_cx -= 8; return; }

    const u8 *glyph = font8x16[(u8)c];
    fb_draw_glyph(glyph, fb_cx, fb_cy, fb_current_fg, fb_bg);
    fb_cx += 8;

    if (fb_cx + 8 > fb_width) { fb_cx = 0; fb_cy += 16; }
    if (fb_cy + 16 > fb_height) {
        u32 line = 16 * fb_pitch;
        u8 *dst = fb_addr;
        u8 *src = fb_addr + line;
        memmove(dst, src, (u64)fb_pitch * (fb_height - 16));
        for (u32 y = fb_height - 16; y < fb_height; y++)
            for (u32 x = 0; x < fb_width; x++)
                fb_putpixel(x, y, fb_bg);
        fb_cy -= 16;
    }
}

/* Public API */
void console_init(void)
{
    serial_init();
    console_clear();
    design_init();
}

void console_set_color(u8 attr) { vga_attr = attr; }
u8   console_get_color(void) { return vga_attr; }

/* Set framebuffer drawing color (32-bit ARGB) */
void console_set_fb_color(u32 color) { fb_current_fg = color; }
u32  console_get_fb_color(void) { return fb_current_fg; }

void console_clear(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEM[i] = (u16)(vga_attr << 8) | ' ';
    cursor_row = cursor_col = 0;
    fb_cx = fb_cy = 0;
    if (fb_addr) {
        for (u32 y = 0; y < fb_height; y++)
            for (u32 x = 0; x < fb_width; x++)
                fb_putpixel(x, y, fb_bg);
    }
}

void console_attach_fb(void *addr, u32 width, u32 height, u32 pitch, u32 bpp)
{
    fb_addr = addr;
    fb_width = width;
    fb_height = height;
    fb_pitch = pitch;
    fb_bpp = bpp;
    fb_cx = fb_cy = 0;
    fb_current_fg = fb_fg;
    if (fb_addr) {
        for (u32 y = 0; y < fb_height; y++)
            for (u32 x = 0; x < fb_width; x++)
                fb_putpixel(x, y, fb_bg);
    }
    design_apply_fb_palette();
}

int console_has_fb(void) { return fb_addr != NULL; }

/* Write a character to all sinks with current style */
void console_putc(char c)
{
    serial_putc(c);
    vga_putc(c);
    fb_putc(c);
}

/* Write styled text to framebuffer with color, plain to serial/VGA */
void console_write_styled(const char *s, u32 fb_color)
{
    u32 old_fg = fb_current_fg;
    fb_current_fg = fb_color;
    while (*s) console_putc(*s++);
    fb_current_fg = old_fg;
}

void console_write(const char *s)
{
    while (*s) console_putc(*s++);
}

void console_vprintf(const char *fmt, void *args)
{
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    console_write(buf);
}

void console_printf(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    console_write(buf);
}

/* Logging with semantic styles — impeccable colorize */
void klog(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    console_write(buf);
    console_putc('\n');
}

void klog_ok(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_write("[ ");
    console_set_color(ATTR_OK);
    console_write("OK");
    console_set_color(old);
    console_write(" ] ");
    console_write(buf);
    console_putc('\n');
    if (console_has_fb()) {
        console_write_styled("[ ", FB_MUTED);
        console_write_styled("OK", FB_OK);
        console_write_styled(" ] ", FB_MUTED);
        console_write_styled(buf, FB_FG);
        console_putc('\n');
    }
}

void klog_warn(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_write("[ ");
    console_set_color(ATTR_WARN);
    console_write("WARN");
    console_set_color(old);
    console_write(" ] ");
    console_write(buf);
    console_putc('\n');
    if (console_has_fb()) {
        console_write_styled("[ ", FB_MUTED);
        console_write_styled("WARN", FB_WARN);
        console_write_styled(" ] ", FB_MUTED);
        console_write_styled(buf, FB_FG);
        console_putc('\n');
    }
}

void klog_err(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_write("[ ");
    console_set_color(ATTR_ERR);
    console_write("ERR");
    console_set_color(old);
    console_write(" ] ");
    console_write(buf);
    console_putc('\n');
    if (console_has_fb()) {
        console_write_styled("[ ", FB_MUTED);
        console_write_styled("ERR", FB_ERR);
        console_write_styled(" ] ", FB_MUTED);
        console_write_styled(buf, FB_FG);
        console_putc('\n');
    }
}

void klog_info(const char *tag, const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_printf("[%s] ", tag);
    console_set_color(ATTR_INFO);
    console_write(buf);
    console_set_color(old);
    console_putc('\n');
    if (console_has_fb()) {
        console_write_styled("[", FB_MUTED);
        console_write_styled(tag, FB_INFO);
        console_write_styled("] ", FB_MUTED);
        console_write_styled(buf, FB_FG);
        console_putc('\n');
    }
}

/* Print a horizontal rule — impeccable quieter: subtle separator */
void console_hr(void)
{
    u8 old = console_get_color();
    console_set_color(ATTR_SEPARATOR);
    console_write("────────────────────────────────────────────────────────────\n");
    console_set_color(old);
    if (console_has_fb()) {
        u32 old_fg = fb_current_fg;
        fb_current_fg = FB_BORDER;
        for (int i = 0; i < 60; i++) console_putc('─');
        console_putc('\n');
        fb_current_fg = old_fg;
    }
}