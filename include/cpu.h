#ifndef NEO_CPU_H
#define NEO_CPU_H

#include <nitekernel.h>

/* Register frame pushed by the ISR stubs. Field order mirrors the memory
   layout produced by isr_common (lowest address first). */
struct registers {
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
} __attribute__((packed));

typedef void (*irq_handler_t)(struct registers *regs);

/* GDT */
void gdt_init(void);
void tss_set_kernel_stack(u64 rsp0);

/* IDT / interrupts */
void idt_init(void);
void idt_set_gate(int vec, u64 handler, u8 dpl);
void interrupts_enable(void);
void interrupts_disable(void);
void irq_install_handler(int irq, irq_handler_t handler);
void irq_uninstall_handler(int irq);
u64  irq_get_ticks(void);
u64  irq_ticks_to_ms(u64 ticks);

/* 8259 PIC */
void pic_init(void);
void pic_remap(int offset1, int offset2);
void pic_send_eoi(int irq);
void pic_mask(int irq);
void pic_unmask(int irq);

/* PIT */
void pit_init(u32 frequency_hz);
u32  pit_get_frequency(void);

/* Panic / diagnostics */
void panic(const char *fmt, ...) __attribute__((noreturn));
void dump_registers(struct registers *r);

/* User mode (ring 3) entry: builds an iretq frame and drops privilege. */
void usermode_enter(u64 entry, u64 user_rsp) __attribute__((noreturn));

#endif
