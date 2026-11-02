#include <design.h>
#include <console.h>
#include <string.h>
#include <stdarg.h>

/* Current framebuffer palette (updated by design_apply_fb_palette) */
static u32 fb_palette_fg   = FB_FG;
static u32 fb_palette_bg   = FB_BG;
static u32 fb_palette_muted = FB_MUTED;
static u32 fb_palette_accent = FB_ACCENT;
static u32 fb_palette_ok   = FB_OK;
static u32 fb_palette_warn = FB_WARN;
static u32 fb_palette_err  = FB_ERR;
static u32 fb_palette_info = FB_INFO;

void design_init(void)
{
    /* VGA defaults are already set in console.c */
    design_apply_fb_palette();
}

void design_apply_fb_palette(void)
{
    fb_palette_fg     = FB_FG;
    fb_palette_bg     = FB_BG;
    fb_palette_muted  = FB_MUTED;
    fb_palette_accent = FB_ACCENT;
    fb_palette_ok     = FB_OK;
    fb_palette_warn   = FB_WARN;
    fb_palette_err    = FB_ERR;
    fb_palette_info   = FB_INFO;

    /* Update console framebuffer colors if attached */
    if (console_has_fb()) {
        extern u32 fb_fg, fb_bg;
        fb_fg = fb_palette_fg;
        fb_bg = fb_palette_bg;
    }
}

u8 design_attr(text_role_t role, int emphasis)
{
    switch (role) {
        case ROLE_DISPLAY:
            return emphasis ? VGA_ATTR(VGA_WHITE, VGA_BLACK) : VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
        case ROLE_TITLE:
            return emphasis ? VGA_ATTR(VGA_LIGHT_CYAN, VGA_BLACK) : VGA_ATTR(VGA_WHITE, VGA_BLACK);
        case ROLE_BODY:
            return emphasis ? VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK) : VGA_ATTR(VGA_WHITE, VGA_BLACK);
        case ROLE_LABEL:
            return emphasis ? VGA_ATTR(VGA_LIGHT_CYAN, VGA_BLACK) : VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
        case ROLE_DATA:
            return emphasis ? VGA_ATTR(VGA_YELLOW, VGA_BLACK) : VGA_ATTR(VGA_WHITE, VGA_BLACK);
        case ROLE_META:
            return emphasis ? VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK) : VGA_ATTR(VGA_DARK_GREY, VGA_BLACK);
        case ROLE_CODE:
            return emphasis ? VGA_ATTR(VGA_LIGHT_GREEN, VGA_BLACK) : VGA_ATTR(VGA_GREEN, VGA_BLACK);
        default:
            return ATTR_PRIMARY;
    }
}

u32 design_fb_color(text_role_t role, int emphasis)
{
    (void)emphasis;
    switch (role) {
        case ROLE_DISPLAY: return fb_palette_fg;
        case ROLE_TITLE:   return fb_palette_accent;
        case ROLE_BODY:    return fb_palette_fg;
        case ROLE_LABEL:   return fb_palette_accent;
        case ROLE_DATA:    return fb_palette_fg;
        case ROLE_META:    return fb_palette_muted;
        case ROLE_CODE:    return fb_palette_ok;
        default:           return fb_palette_fg;
    }
}

/* --- Semantic print helpers --- */

void design_print_title(const char *text)
{
    u8 old = console_get_color();
    console_set_color(ATTR_TITLE);
    console_write(text);
    console_set_color(old);
    console_putc('\n');
}

void design_print_subtitle(const char *text)
{
    u8 old = console_get_color();
    console_set_color(ATTR_SUBTITLE);
    console_write(text);
    console_set_color(old);
    console_putc('\n');
}

void design_print_body(const char *text)
{
    console_write(text);
    console_putc('\n');
}

void design_print_label(const char *label, const char *value)
{
    u8 old = console_get_color();
    console_set_color(ATTR_LABEL);
    console_write(label);
    console_set_color(ATTR_DATA);
    console_write(value);
    console_set_color(old);
    console_putc('\n');
}

void design_print_kv(const char *key, const char *value)
{
    u8 old = console_get_color();
    console_set_color(ATTR_LABEL);
    console_write(key);
    console_write(": ");
    console_set_color(ATTR_DATA);
    console_write(value);
    console_set_color(old);
    console_putc('\n');
}

void design_print_ok(const char *fmt, ...)
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
}

void design_print_warn(const char *fmt, ...)
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
}

void design_print_err(const char *fmt, ...)
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
}

void design_print_info(const char *fmt, ...)
{
    char buf[1024];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, &ap);
    __builtin_va_end(ap);

    u8 old = console_get_color();
    console_write("[ ");
    console_set_color(ATTR_INFO);
    console_write("INFO");
    console_set_color(old);
    console_write(" ] ");
    console_write(buf);
    console_putc('\n');
}

void design_print_separator(void)
{
    u8 old = console_get_color();
    console_set_color(ATTR_SEPARATOR);
    console_write("────────────────────────────────────────\n");
    console_set_color(old);
}

void design_print_box(const char *title, const char *content)
{
    u8 old = console_get_color();
    console_set_color(ATTR_BORDER);

    console_write("┌─ ");
    console_set_color(ATTR_TITLE);
    console_write(title);
    console_set_color(ATTR_BORDER);
    console_write(" ");
    for (int i = 0; i < 40 - (int)strlen(title); i++) console_write("─");
    console_write("┐\n");

    console_set_color(ATTR_BODY);
    console_write(content);
    console_putc('\n');

    console_set_color(ATTR_BORDER);
    console_write("└");
    for (int i = 0; i < 44; i++) console_write("─");
    console_write("┘\n");

    console_set_color(old);
}