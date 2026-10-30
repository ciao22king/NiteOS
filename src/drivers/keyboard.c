#include <input.h>
#include <cpu.h>
#include <console.h>
#include <string.h>

#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_IRQ 1

/* Ring buffer of decoded characters. */
static volatile char buf[KBD_BUF_SIZE];
static volatile u32 head, tail;
static volatile u32 flags;
static volatile u32 pending_char;      /* 1 when a key is waiting */
static int echo;

static const char kbd_normal[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/',
    0,   '*', 0,  ' ',
};

static const char kbd_shift[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?',
    0,   '*', 0,  ' ',
};

static void push(char c)
{
    u32 next = (head + 1) % KBD_BUF_SIZE;
    if (next == tail) return;    /* full: drop */
    buf[head] = c;
    head = next;
}

static void keyboard_irq(struct registers *regs)
{
    (void)regs;
    u8 scancode = inb(KBD_DATA_PORT);

    if (scancode == 0xE0) { flags |= KEY_EXTENDED; return; }
    int released = scancode & 0x80;
    u8 code = scancode & 0x7F;
    u32 old_flags = flags;

    if (code == 0x2A || code == 0x36) {
        if (released) flags &= ~KEY_SHIFT; else flags |= KEY_SHIFT;
        flags &= ~KEY_EXTENDED;
        return;
    }
    if (code == 0x1D) {
        if (released) flags &= ~KEY_CTRL; else flags |= KEY_CTRL;
        flags &= ~KEY_EXTENDED;
        return;
    }
    if (code == 0x38) {
        if (released) flags &= ~KEY_ALT; else flags |= KEY_ALT;
        flags &= ~KEY_EXTENDED;
        return;
    }
    if (code == 0x3A && !released) {
        flags ^= KEY_CAPS;
        flags &= ~KEY_EXTENDED;
        return;
    }
    if (released) { flags &= ~KEY_EXTENDED; return; }

    char c = 0;
    if (code < 128) {
        int upper = (flags & KEY_SHIFT) != 0;
        if (flags & KEY_CAPS) upper = !upper;
        c = upper ? kbd_shift[code] : kbd_normal[code];
        if (c == 0 && code < 58) c = kbd_normal[code];   /* fallback */
    }

    if (c) {
        if ((flags & KEY_CTRL) && c >= 'a' && c <= 'z') c -= 32;   /* Ctrl+A..Z */
        if ((flags & KEY_CTRL) && c == 'c') c = 3;
        push(c);
        pending_char = 1;
        if (echo) console_putc(c);
    }
    flags &= ~KEY_EXTENDED;
    (void)old_flags;
}

void keyboard_init(void)
{
    head = tail = 0;
    flags = 0;
    pending_char = 0;
    echo = 1;

    /* Drain any bytes the controller is holding. */
    while (inb(KBD_STATUS_PORT) & 0x01) (void)inb(KBD_DATA_PORT);

    irq_install_handler(KBD_IRQ, keyboard_irq);
    pic_unmask(KBD_IRQ);
}

int keyboard_has_data(void) { return head != tail; }

int keyboard_try_getchar(void)
{
    if (head == tail) return -1;
    char c = buf[tail];
    tail = (tail + 1) % KBD_BUF_SIZE;
    return (u8)c;
}

int keyboard_getchar(void)
{
    int c;
    while ((c = keyboard_try_getchar()) < 0) hlt();
    return c;
}

void keyboard_flush(void) { head = tail = 0; }
void keyboard_set_echo(int on) { echo = on; }
