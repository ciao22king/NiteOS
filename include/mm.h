#ifndef NEO_MM_H
#define NEO_MM_H

#include <nitekernel.h>
#include <multiboot2.h>

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1))
/* Physical-address bits of a page-table entry. PAGE_MASK alone is unsafe for
   extraction: it leaves the NX bit (63) and the software bits (52-62) set, so
   a non-executable mapping yields a non-canonical "physical" address. */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Page table entry flags */
#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITE    (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)
#define PAGE_NX       (1ULL << 63)

/* The direct map (0xFFFFFFFF80000000 + phys) covers this much RAM. */
#define DIRECT_MAP_SIZE (1ULL << 30)

/* ---------------- Physical memory manager (bitmap) ---------------- */
void  pmm_init(struct mb2_tag_mmap *mmap, u64 kernel_end_phys, u64 mbi_end_phys);
u64   pmm_alloc_page(void);
u64   pmm_alloc_pages(usize count);
void  pmm_free_page(u64 phys);
void  pmm_free_pages(u64 phys, usize count);
void  pmm_reserve(u64 phys, u64 len);
void  pmm_release(u64 phys, u64 len);
usize pmm_total_bytes(void);
usize pmm_free_bytes(void);
usize pmm_used_bytes(void);
int   pmm_is_managed(u64 phys);

/* Per-process address space: a PML4 with the kernel half shared. */
struct addr_space;
struct addr_space *vmm_new_space(void);
void  vmm_free_space(struct addr_space *as);
void  vmm_switch_space(struct addr_space *as);
struct addr_space *vmm_kernel_space(void);
int   vmm_space_map(struct addr_space *as, u64 virt, u64 phys, u64 flags);
int   vmm_space_unmap(struct addr_space *as, u64 virt);
int   vmm_space_alloc(struct addr_space *as, u64 virt, u64 len, u64 flags);
u64   vmm_space_get_phys(struct addr_space *as, u64 virt);
u64   vmm_space_cr3(struct addr_space *as);

/* ---------------- Virtual memory manager ---------------- */
void  vmm_init(void);
int   vmm_map(u64 virt, u64 phys, u64 flags);
int   vmm_map_range(u64 virt, u64 phys, u64 len, u64 flags);
int   vmm_unmap(u64 virt);
u64   vmm_get_phys(u64 virt);
void  vmm_stats(usize *mapped_pages);

/* ---------------- Kernel heap ---------------- */
void  heap_init(void);
void *kmalloc(usize size);
void *kzalloc(usize size);
void *krealloc(void *ptr, usize size);
void  kfree(void *ptr);
void  heap_stats(usize *total, usize *used, usize *free_bytes);

#endif
