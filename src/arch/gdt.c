#include <cpu.h>
#include <string.h>
#include <console.h>

/* 64-bit GDT: null, kernel code, kernel data, user code, user data, TSS. */
#define GDT_ENTRIES 7

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct tss_entry {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed));

static u64 gdt[GDT_ENTRIES];
static struct gdt_ptr gdtp;
static struct tss_entry tss;

extern void gdt_flush(struct gdt_ptr *ptr);

static void set_entry(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i] = ((u64)(limit & 0xFFFF)) |
             ((u64)(base & 0xFFFFFF) << 16) |
             ((u64)access << 40) |
             ((u64)((limit >> 16) & 0x0F) << 48) |
             ((u64)(flags & 0xF0) << 48) |
             ((u64)((base >> 24) & 0xFF) << 56);
}

static void write_tss(int i, u64 base, u32 limit)
{
    set_entry(i, (u32)base, limit, 0x89, 0x00);
    /* upper 32 bits of the 64-bit base live in the next slot */
    gdt[i + 1] = base >> 32;
}

void gdt_init(void)
{
    memset(&tss, 0, sizeof(tss));
    tss.iomap_base = sizeof(tss);

    set_entry(0, 0, 0, 0, 0);
    set_entry(1, 0, 0, 0x9A, 0xA0);   /* 0x08 kernel code: L=1 */
    set_entry(2, 0, 0, 0x92, 0xC0);   /* 0x10 kernel data */
    /* Order matters for sysretq: it derives SS from STAR+8 and CS from
       STAR+16, so user data must sit immediately before user code. */
    set_entry(3, 0, 0, 0xF2, 0xC0);   /* 0x18 user data */
    set_entry(4, 0, 0, 0xFA, 0xA0);   /* 0x20 user code */
    write_tss(5, (u64)&tss, sizeof(tss) - 1);  /* 0x28 TSS */

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (u64)&gdt;

    gdt_flush(&gdtp);
    __asm__ volatile("ltr %%ax" ::"a"((u16)0x28));
}

void tss_set_kernel_stack(u64 rsp0)
{
    tss.rsp0 = rsp0;
}
