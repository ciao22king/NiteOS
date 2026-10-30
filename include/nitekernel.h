/* NiteKernel - common types and build metadata. */
#ifndef NITEKERNEL_H
#define NITEKERNEL_H

#define NITEKERNEL_VERSION "1.0.0"
#define NITEKERNEL_NAME    "NiteKernel"

typedef unsigned char      u8;
typedef signed char        i8;
typedef unsigned short     u16;
typedef signed short       i16;
typedef unsigned int       u32;
typedef signed int         i32;
typedef unsigned long long u64;
typedef signed long long   i64;
typedef unsigned long      usize;
typedef signed long        isize;

#define NULL ((void *)0)
#define KERNEL_VMA 0xFFFFFFFF80000000ULL

/* Minimal stddef replacements (no hosted headers in a freestanding build). */
typedef __SIZE_TYPE__ size_t;
#define offsetof(type, member) __builtin_offsetof(type, member)

/* Physical <-> virtual helpers for the kernel image region. */
static inline u64 phys_to_virt(u64 p) { return p + KERNEL_VMA; }
static inline u64 virt_to_phys(u64 v) { return v - KERNEL_VMA; }

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static inline void outb(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" ::"a"(val), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" ::"a"(val), "Nd"(port));
}
static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(u16 port, u32 val)
{
    __asm__ volatile("outl %0, %1" ::"a"(val), "Nd"(port));
}
static inline u32 inl(u16 port)
{
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

#endif /* NITEKERNEL_H */
