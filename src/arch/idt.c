#include <cpu.h>
#include <string.h>
#include <console.h>

#define IDT_ENTRIES 256

struct idt_entry {
    u16 off_lo;
    u16 selector;
    u8  ist;
    u8  type_attr;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

#define ISR_LIST \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  X(8)  X(9)  X(10) X(11) \
    X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35) \
    X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)

#define X(n) extern void isr##n(void);
ISR_LIST
#undef X

#define X(n) isr##n,
static void (*isr_stubs[48])(void) = { ISR_LIST };
#undef X

static irq_handler_t irq_handlers[16];
static volatile u64 tick_count;
static u32 pit_frequency = 100;

static const char *exception_names[32] = {
    "Divide-by-zero", "Debug", "Non-maskable interrupt", "Breakpoint",
    "Overflow", "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating-point", "Alignment check",
    "Machine check", "SIMD floating-point", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor injection", "VMM communication",
    "Security", "Reserved"
};

void dump_registers(struct registers *r)
{
    klog_err("  RAX=%p RBX=%p RCX=%p RDX=%p", (void *)r->rax, (void *)r->rbx,
             (void *)r->rcx, (void *)r->rdx);
    klog_err("  RSI=%p RDI=%p RBP=%p RSP=%p", (void *)r->rsi, (void *)r->rdi,
             (void *)r->rbp, (void *)r->rsp);
    klog_err("  R8 =%p R9 =%p R10=%p R11=%p", (void *)r->r8, (void *)r->r9,
             (void *)r->r10, (void *)r->r11);
    klog_err("  RIP=%p CS=%p RFLAGS=%p ERR=%p", (void *)r->rip, (void *)r->cs,
             (void *)r->rflags, (void *)r->err_code);
}

void isr_dispatch(struct registers *r)
{
    if (r->int_no < 32) {
        const char *name = exception_names[r->int_no];
        console_set_color(VGA_ATTR(VGA_LIGHT_RED, VGA_BLACK));
        klog_err("EXCEPTION %u: %s (error code %p)", (u32)r->int_no, name,
                 (void *)r->err_code);
        if (r->int_no == 14) {
            u64 cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            klog_err("  faulting address (CR2) = %p", (void *)cr2);
        }
        dump_registers(r);
        console_set_color(VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK));
        panic("unhandled CPU exception");
    }

    int irq = (int)r->int_no - 32;
    if (irq >= 0 && irq < 16) {
        if (irq_handlers[irq]) irq_handlers[irq](r);
        pic_send_eoi(irq);
    }
}

void irq_install_handler(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16) irq_handlers[irq] = handler;
}

void irq_uninstall_handler(int irq)
{
    if (irq >= 0 && irq < 16) irq_handlers[irq] = NULL;
}

u64 irq_get_ticks(void) { return tick_count; }

u64 irq_ticks_to_ms(u64 ticks)
{
    if (!pit_frequency) return 0;
    return ticks * 1000 / pit_frequency;
}

static void timer_irq(struct registers *r)
{
    (void)r;
    tick_count++;
}

void idt_set_gate(int vec, u64 handler, u8 dpl)
{
    if (vec < 0 || vec >= IDT_ENTRIES) return;
    idt[vec].off_lo = handler & 0xFFFF;
    idt[vec].off_mid = (handler >> 16) & 0xFFFF;
    idt[vec].off_hi = (handler >> 32) & 0xFFFFFFFF;
    idt[vec].selector = 0x08;
    idt[vec].ist = 0;
    idt[vec].type_attr = (u8)(0x8E | ((dpl & 3) << 5));
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));

    for (int i = 0; i < 48; i++) {
        u64 base = (u64)isr_stubs[i];
        idt[i].off_lo = base & 0xFFFF;
        idt[i].off_mid = (base >> 16) & 0xFFFF;
        idt[i].off_hi = (base >> 32) & 0xFFFFFFFF;
        idt[i].selector = 0x08;
        idt[i].ist = 0;
        idt[i].type_attr = 0x8E;   /* present, ring 0, 64-bit interrupt gate */
    }

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (u64)&idt;
    __asm__ volatile("lidt %0" ::"m"(idtp));

    pit_init(100);
    irq_install_handler(0, timer_irq);
    pic_unmask(0);
}

void interrupts_enable(void) { sti(); }
void interrupts_disable(void) { cli(); }
