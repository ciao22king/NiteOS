#ifndef NEO_INPUT_H
#define NEO_INPUT_H

#include <nitekernel.h>

#define KBD_BUF_SIZE 256

/* Key event flags */
#define KEY_SHIFT   (1u << 0)
#define KEY_CTRL    (1u << 1)
#define KEY_ALT     (1u << 2)
#define KEY_CAPS    (1u << 3)
#define KEY_EXTENDED (1u << 4)

/* Line-editing codes delivered through the same character stream as text, so
   a terminal and a PS/2 keyboard drive the shell identically. They match the
   control codes a terminal sends for Ctrl+P / Ctrl+N. */
#define KEY_UP   0x10
#define KEY_DOWN 0x0E

struct key_event {
    char ascii;      /* translated character, 0 for non-printable */
    u8   scancode;   /* raw make/break code */
    u8   pressed;    /* 1 = make, 0 = break */
    u32  flags;
};

void  keyboard_init(void);
int   keyboard_has_data(void);
int   keyboard_getchar(void);          /* blocking; -1 if nothing pending (non-blocking) */
int   keyboard_try_getchar(void);
void  keyboard_flush(void);
void  keyboard_set_echo(int on);

#endif
