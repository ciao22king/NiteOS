#include <cpu.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01

void pic_remap(int offset1, int offset2)
{
    u8 mask1 = inb(PIC1_DATA);
    u8 mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, (u8)offset1);
    io_wait();
    outb(PIC2_DATA, (u8)offset2);
    io_wait();

    outb(PIC1_DATA, 4);   /* slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 2);   /* cascade identity */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_init(void)
{
    pic_remap(32, 40);
    /* Mask everything; drivers unmask what they need. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(int irq)
{
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask(int irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    int bit = irq < 8 ? irq : irq - 8;
    outb(port, (u8)(inb(port) | (1 << bit)));
}

void pic_unmask(int irq)
{
    u16 port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    int bit = irq < 8 ? irq : irq - 8;
    outb(port, (u8)(inb(port) & ~(1 << bit)));
}
