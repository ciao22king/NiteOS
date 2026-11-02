#ifndef NEO_DESIGN_H
#define NEO_DESIGN_H

#include <nitekernel.h>

/* ============================================================================
 * IMPECCABLE DESIGN TOKENS — NiteKernel Console
 * Semantic design system for a bare-metal kernel shell
 * ============================================================================ */

/* ---------------------------------------------------------------------------
 * COLOR SYSTEM — Semantic roles mapped to VGA 16-color palette
 * Following impeccable/colorize: color encodes hierarchy, state, meaning
 * --------------------------------------------------------------------------- */

/* Canvas & surfaces */
#define COLOR_CANVAS        VGA_BLACK
#define COLOR_SURFACE       VGA_DARK_GREY
#define COLOR_SURFACE_RAISED VGA_BLACK

/* Text hierarchy */
#define COLOR_TEXT_PRIMARY   VGA_WHITE
#define COLOR_TEXT_SECONDARY VGA_LIGHT_GREY
#define COLOR_TEXT_MUTED     VGA_DARK_GREY
#define COLOR_TEXT_INVERSE   VGA_BLACK

/* Action & interactive */
#define COLOR_ACTION         VGA_LIGHT_CYAN
#define COLOR_ACTION_HOVER   VGA_CYAN
#define COLOR_FOCUS          VGA_YELLOW

/* Semantic states — consistent meanings across the shell */
#define COLOR_SUCCESS        VGA_LIGHT_GREEN
#define COLOR_WARNING        VGA_YELLOW
#define COLOR_ERROR          VGA_LIGHT_RED
#define COLOR_INFO           VGA_LIGHT_BLUE
#define COLOR_ACCENT         VGA_LIGHT_MAGENTA

/* Borders & separators */
#define COLOR_BORDER         VGA_DARK_GREY
#define COLOR_SEPARATOR      VGA_DARK_GREY

/* ---------------------------------------------------------------------------
 * COMPOSED VGA ATTRIBUTES — Ready-to-use attribute bytes
 * Format: (bg << 4) | fg
 * --------------------------------------------------------------------------- */

/* Text styles */
#define ATTR_PRIMARY         VGA_ATTR(COLOR_TEXT_PRIMARY,   COLOR_CANVAS)
#define ATTR_SECONDARY       VGA_ATTR(COLOR_TEXT_SECONDARY, COLOR_CANVAS)
#define ATTR_MUTED           VGA_ATTR(COLOR_TEXT_MUTED,     COLOR_CANVAS)
#define ATTR_INVERSE         VGA_ATTR(COLOR_TEXT_INVERSE,   COLOR_TEXT_PRIMARY)

/* Semantic message styles */
#define ATTR_OK              VGA_ATTR(COLOR_SUCCESS,        COLOR_CANVAS)
#define ATTR_WARN            VGA_ATTR(COLOR_WARNING,        COLOR_CANVAS)
#define ATTR_ERR             VGA_ATTR(COLOR_ERROR,          COLOR_CANVAS)
#define ATTR_INFO            VGA_ATTR(COLOR_INFO,           COLOR_CANVAS)
#define ATTR_ACCENT          VGA_ATTR(COLOR_ACCENT,         COLOR_CANVAS)

/* Interactive elements */
#define ATTR_PROMPT          VGA_ATTR(COLOR_ACTION,         COLOR_CANVAS)
#define ATTR_PROMPT_PATH     VGA_ATTR(COLOR_TEXT_SECONDARY, COLOR_CANVAS)
#define ATTR_COMMAND         VGA_ATTR(COLOR_TEXT_PRIMARY,   COLOR_CANVAS)
#define ATTR_ARGUMENT        VGA_ATTR(COLOR_TEXT_SECONDARY, COLOR_CANVAS)
#define ATTR_BUILTIN         VGA_ATTR(COLOR_ACCENT,         COLOR_CANVAS)

/* UI chrome */
#define ATTR_BORDER          VGA_ATTR(COLOR_BORDER,         COLOR_CANVAS)
#define ATTR_SEPARATOR       VGA_ATTR(COLOR_SEPARATOR,      COLOR_CANVAS)
#define ATTR_TITLE           VGA_ATTR(COLOR_TEXT_PRIMARY,   COLOR_CANVAS)
#define ATTR_SUBTITLE        VGA_ATTR(COLOR_TEXT_SECONDARY, COLOR_CANVAS)

/* Highlight / selection */
#define ATTR_HIGHLIGHT       VGA_ATTR(COLOR_TEXT_INVERSE,   COLOR_ACTION)
#define ATTR_SELECTION       VGA_ATTR(COLOR_TEXT_INVERSE,   COLOR_FOCUS)

/* ---------------------------------------------------------------------------
 * FRAMEBUFFER COLOR PALETTE — 32-bit ARGB for linear framebuffer
 * Following impeccable/typeset: perceptually tuned for dark surfaces
 * --------------------------------------------------------------------------- */

#define FB_BG        0x0A0A14FF  /* Deep charcoal, not pure black */
#define FB_FG        0xE8E8E8FF  /* Soft white, reduced glare */
#define FB_MUTED     0x6A6A7AFF  /* Muted grey for secondary text */
#define FB_ACCENT    0x00D4FFFF  /* Cyan accent */
#define FB_OK        0x4ADE80FF  /* Success green */
#define FB_WARN      0xFBBF24FF  /* Warning amber */
#define FB_ERR       0xF87171FF  /* Error red */
#define FB_INFO      0x60A5FAFF  /* Info blue */
#define FB_SELECTION 0x00D4FF40  /* Accent with transparency */
#define FB_BORDER    0x2A2A3AFF  /* Subtle border */

/* ---------------------------------------------------------------------------
 * TYPOGRAPHY SCALE — Following impeccable/typeset
 * Role-based, not arbitrary sizes
 * --------------------------------------------------------------------------- */

typedef enum {
    ROLE_DISPLAY   = 0,  /* Boot banner, major headers */
    ROLE_TITLE     = 1,  /* Section titles, command groups */
    ROLE_BODY      = 2,  /* Default output, help text */
    ROLE_LABEL     = 3,  /* Key-value labels, flags */
    ROLE_DATA      = 4,  /* Values, numbers, paths */
    ROLE_META      = 5,  /* Timestamps, hints, footnotes */
    ROLE_CODE      = 6,  /* Monospace: commands, output */
} text_role_t;

/* Framebuffer line heights per role (in pixels, 8x16 font base) */
#define FB_LH_DISPLAY  24
#define FB_LH_TITLE    20
#define FB_LH_BODY     18
#define FB_LH_LABEL    17
#define FB_LH_DATA     18
#define FB_LH_META     16
#define FB_LH_CODE     18

/* ---------------------------------------------------------------------------
 * SPACING SYSTEM — Consistent rhythm
 * --------------------------------------------------------------------------- */

#define SPACE_XS  1   /* Tight: between label:value */
#define SPACE_SM  2   /* Compact: list items */
#define SPACE_MD  4   /* Default: paragraph breaks */
#define SPACE_LG  8   /* Generous: section breaks */
#define SPACE_XL  16  /* Major: screen regions */

/* ---------------------------------------------------------------------------
 * PUBLIC API
 * --------------------------------------------------------------------------- */

void design_init(void);
void design_apply_fb_palette(void);

/* Get attribute for a semantic role */
u8 design_attr(text_role_t role, int emphasis);  /* emphasis: 0=normal, 1=bold/bright */

/* Get framebuffer color for a semantic role */
u32 design_fb_color(text_role_t role, int emphasis);

/* Print helpers with semantic styling */
void design_print_title(const char *text);
void design_print_subtitle(const char *text);
void design_print_body(const char *text);
void design_print_label(const char *label, const char *value);
void design_print_kv(const char *key, const char *value);
void design_print_ok(const char *fmt, ...);
void design_print_warn(const char *fmt, ...);
void design_print_err(const char *fmt, ...);
void design_print_info(const char *fmt, ...);
void design_print_separator(void);
void design_print_box(const char *title, const char *content);

#endif