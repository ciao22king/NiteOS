#ifndef NEO_GFX_H
#define NEO_GFX_H

#include <nitekernel.h>
#include <devices.h>

struct gfx_mode {
    u64  fb_phys;
    void *fb_virt;
    u32  width, height, pitch, bpp;
    u32  bytes_per_pixel;
    int  ready;
    char driver[32];
};

/* Probe the display adapter, set a linear graphics mode and map the
   framebuffer into kernel virtual space. Returns 0 on success. */
int  gfx_init(struct hw_profile *hw);
const struct gfx_mode *gfx_get_mode(void);

/* Basic drawing primitives used by the UI. */
void gfx_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void gfx_draw_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void gfx_putpixel(u32 x, u32 y, u32 color);
void gfx_clear(u32 color);
void gfx_scroll_up(u32 pixels, u32 fill_color);
u32  gfx_rgb(u8 r, u8 g, u8 b);

#endif
