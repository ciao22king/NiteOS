#include <cpu.h>

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_BASE  1193182

static u32 pit_hz = 100;

void pit_init(u32 frequency_hz)
{
    if (frequency_hz == 0) frequency_hz = 100;
    pit_hz = frequency_hz;

    u32 divisor = PIT_BASE / frequency_hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor == 0) divisor = 1;

    outb(PIT_CMD, 0x36);                  /* channel 0, lo/hi, mode 3, binary */
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));
}

u32 pit_get_frequency(void) { return pit_hz; }
