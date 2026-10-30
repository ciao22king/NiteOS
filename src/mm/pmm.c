#include <mm.h>
#include <string.h>
#include <console.h>

/* Managed physical memory is capped at 1 GiB so the boot-time direct map
   (0xFFFFFFFF80000000 + phys) can reach every page the allocator hands out. */
#define PMM_MAX_MEM   DIRECT_MAP_SIZE
#define PMM_PAGES     (PMM_MAX_MEM / PAGE_SIZE)
#define PMM_BITMAP_SZ (PMM_PAGES / 8)

static u8  bitmap[PMM_BITMAP_SZ];
static u64 total_pages;
static u64 used_pages;
static u64 alloc_hint;

static inline int bit_test(u64 idx) { return (bitmap[idx >> 3] >> (idx & 7)) & 1; }
static inline void bit_set(u64 idx) { bitmap[idx >> 3] |= (u8)(1u << (idx & 7)); }
static inline void bit_clear(u64 idx) { bitmap[idx >> 3] &= (u8)~(1u << (idx & 7)); }

static void mark_used(u64 page) { if (page < total_pages && !bit_test(page)) { bit_set(page); used_pages++; } }
static void mark_free(u64 page) { if (page < total_pages && bit_test(page)) { bit_clear(page); used_pages--; } }

void pmm_reserve(u64 phys, u64 len)
{
    u64 start = ALIGN_DOWN(phys, PAGE_SIZE) / PAGE_SIZE;
    u64 end = ALIGN_UP(phys + len, PAGE_SIZE) / PAGE_SIZE;
    for (u64 p = start; p < end; p++) mark_used(p);
}

void pmm_release(u64 phys, u64 len)
{
    u64 start = ALIGN_DOWN(phys, PAGE_SIZE) / PAGE_SIZE;
    u64 end = ALIGN_UP(phys + len, PAGE_SIZE) / PAGE_SIZE;
    for (u64 p = start; p < end; p++) mark_free(p);
}

int pmm_is_managed(u64 phys)
{
    return (phys < PMM_MAX_MEM) && (phys / PAGE_SIZE) < total_pages;
}

void pmm_init(struct mb2_tag_mmap *mmap, u64 kernel_end_phys, u64 mbi_end_phys)
{
    alloc_hint = 0;
    memset(bitmap, 0xFF, sizeof(bitmap));

    /* total_pages reflects real installed RAM (highest available page), not the
       1 GiB bitmap capacity, so the reported totals are meaningful. */
    u64 highest = 0;
    if (mmap) {
        u8 *p = (u8 *)mmap + sizeof(struct mb2_tag_mmap);
        u8 *end = (u8 *)mmap + mmap->size;
        for (; p + mmap->entry_size <= end; p += mmap->entry_size) {
            struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
            if (e->type != MB2_MEMORY_AVAILABLE) continue;
            if (e->addr >= PMM_MAX_MEM) continue;
            u64 b = ALIGN_DOWN(MIN(e->addr + e->len, PMM_MAX_MEM), PAGE_SIZE);
            if (b > highest) highest = b;
        }
    }
    total_pages = highest / PAGE_SIZE;
    used_pages = total_pages;

    /* Free every page inside an available memory-map region. */
    if (mmap) {
        u8 *p = (u8 *)mmap + sizeof(struct mb2_tag_mmap);
        u8 *end = (u8 *)mmap + mmap->size;
        for (; p + mmap->entry_size <= end; p += mmap->entry_size) {
            struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)p;
            if (e->type != MB2_MEMORY_AVAILABLE) continue;
            if (e->addr >= PMM_MAX_MEM) continue;
            u64 a = ALIGN_UP(e->addr, PAGE_SIZE);
            u64 b = ALIGN_DOWN(MIN(e->addr + e->len, PMM_MAX_MEM), PAGE_SIZE);
            for (u64 addr = a; addr < b; addr += PAGE_SIZE)
                mark_free(addr / PAGE_SIZE);
        }
    }

    /* Reserve the pieces the allocator must never hand out. */
    pmm_reserve(0, 0x100000);                       /* real-mode + BIOS area */
    pmm_reserve(0x100000, kernel_end_phys - 0x100000); /* kernel image + boot data */
    (void)mbi_end_phys;   /* multiboot info is reserved by the caller */
}

u64 pmm_alloc_page(void)
{
    for (u64 i = 0; i < total_pages; i++) {
        u64 idx = (alloc_hint + i) % total_pages;
        if (!bit_test(idx)) {
            bit_set(idx);
            used_pages++;
            alloc_hint = idx + 1;
            return idx * PAGE_SIZE;
        }
    }
    return 0;   /* out of memory (page 0 is never free) */
}

u64 pmm_alloc_pages(usize count)
{
    if (count == 1) return pmm_alloc_page();
    u64 run = 0, start = 0;
    for (u64 idx = 0; idx < total_pages; idx++) {
        if (!bit_test(idx)) {
            if (run == 0) start = idx;
            if (++run == count) {
                for (u64 j = start; j < start + count; j++) { bit_set(j); used_pages++; }
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    return 0;
}

void pmm_free_page(u64 phys)
{
    u64 idx = phys / PAGE_SIZE;
    if (idx < total_pages) mark_free(idx);
}

void pmm_free_pages(u64 phys, usize count)
{
    u64 idx = phys / PAGE_SIZE;
    for (usize i = 0; i < count; i++) mark_free(idx + i);
}

usize pmm_total_bytes(void) { return total_pages * PAGE_SIZE; }
usize pmm_used_bytes(void)  { return used_pages * PAGE_SIZE; }
usize pmm_free_bytes(void)  { return (total_pages - used_pages) * PAGE_SIZE; }
