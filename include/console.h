#ifndef NEO_CONSOLE_H
#define NEO_CONSOLE_H

#include <nitekernel.h>

/* VGA text-mode colours */
enum {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE
};

#define VGA_ATTR(fg, bg) ((u8)(((bg) << 4) | ((fg) & 0x0F)))

void console_init(void);
void console_set_color(u8 attr);
u8   console_get_color(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_printf(const char *fmt, ...);
void console_vprintf(const char *fmt, void *args);

/* Attach a linear framebuffer as an additional console sink. */
void console_attach_fb(void *addr, u32 width, u32 height, u32 pitch, u32 bpp);
int  console_has_fb(void);

/* Output hook, consulted by console_putc() before rendering: returning
   non-zero means the character was consumed elsewhere. The syscall layer uses
   it so shell redirection also captures text that kernel commands and lash
   builtins print through console_printf() instead of write(2). */
typedef int (*console_sink_fn)(char c);
void console_set_sink(console_sink_fn fn);

/* Serial (COM1) input, so the shell is interactive over a headless line. */
int  console_getchar(void);      /* -1 when nothing is pending */
int  console_has_input(void);

/* Log helpers */
void klog(const char *fmt, ...);
void klog_ok(const char *fmt, ...);
void klog_warn(const char *fmt, ...);
void klog_err(const char *fmt, ...);
void klog_info(const char *tag, const char *fmt, ...);

#endif
