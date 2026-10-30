#include <console.h>
#include <string.h>
#include <font8x16.h>

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_MEM  ((volatile u16 *)phys_to_virt(0xB8000))

#define SERIAL_COM1 0x3F8

static u8  vga_attr = VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
static int cursor_row = 0;
static int cursor_col = 0;

/* --- framebuffer sink (set once a linear framebuffer is discovered) --- */
static u8  *fb_addr = NULL;
static u32  fb_width, fb_height, fb_pitch, fb_bpp;
static u32  fb_cx, fb_cy;
static u32  fb_fg = 0x00C8C8C8, fb_bg = 0x000A0A14;

/* --- serial --- */
static void serial_init(void)
{
    outb(SERIAL_COM1 + 1, 0x00);   /* disable interrupts */
    outb(SERIAL_COM1 + 3, 0x80);   /* enable DLAB */
    outb(SERIAL_COM1 + 0, 0x01);   /* divisor low  (115200 baud) */
    outb(SERIAL_COM1 + 1, 0x00);   /* divisor high */
    outb(SERIAL_COM1 + 3, 0x03);   /* 8 bits, no parity, one stop */
    outb(SERIAL_COM1 + 2, 0xC7);   /* enable FIFO, clear, 14-byte threshold */
    outb(SERIAL_COM1 + 4, 0x0B);   /* IRQs enabled, RTS/DSR set */
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

/* --- VGA text --- */
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

/* --- framebuffer text (simple 8x16 bitmap font) --- */
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

static void fb_putc(char c)
{
    if (!fb_addr) return;
    if (c == '\n') { fb_cx = 0; fb_cy += 16; return; }
    if (c == '\r') { fb_cx = 0; return; }
    if (c == '\b') { if (fb_cx >= 8) fb_cx -= 8; return; }

    const u8 *glyph = font8x16[(u8)c];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++)
            fb_putpixel(fb_cx + col, fb_cy + row, (bits & (0x80 >> col)) ? fb_fg : fb_bg);
    }
    fb_cx += 8;
    if (fb_cx + 8 > fb_width) { fb_cx = 0; fb_cy += 16; }
    if (fb_cy + 16 > fb_height) {
        /* scroll up one text line */
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

/* --- public API --- */
void console_init(void)
{
    serial_init();
    console_clear();
}

void console_set_color(u8 attr) { vga_attr = attr; }
u8   console_get_color(void) { return vga_attr; }

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
    if (fb_addr) {
        for (u32 y = 0; y < fb_height; y++)
            for (u32 x = 0; x < fb_width; x++)
                fb_putpixel(x, y, fb_bg);
    }
}

int console_has_fb(void) { return fb_addr != NULL; }

void console_putc(char c)
{
    serial_putc(c);
    vga_putc(c);
    fb_putc(c);
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
    console_write("[ OK ] ");
    console_set_color(VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK));
    console_write(buf);
    console_set_color(old);
    console_putc('\n');
}

void klog_warn(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_write("[WARN] ");
    console_set_color(VGA_ATTR(VGA_YELLOW, VGA_BLACK));
    console_write(buf);
    console_set_color(old);
    console_putc('\n');
}

void klog_err(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);
    u8 old = console_get_color();
    console_write("[FAIL] ");
    console_set_color(VGA_ATTR(VGA_LIGHT_RED, VGA_BLACK));
    console_write(buf);
    console_set_color(old);
    console_putc('\n');
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
    console_set_color(VGA_ATTR(VGA_LIGHT_CYAN, VGA_BLACK));
    console_write(buf);
    console_set_color(old);
    console_putc('\n');
}
