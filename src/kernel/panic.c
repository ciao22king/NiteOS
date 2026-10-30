#include <cpu.h>
#include <console.h>
#include <string.h>

void panic(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);

    cli();
    console_set_color(VGA_ATTR(VGA_WHITE, VGA_RED));
    console_write("\n*** KERNEL PANIC *** ");
    console_write(buf);
    console_write(" ***\n");
    console_set_color(VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));

    for (;;) hlt();
}
