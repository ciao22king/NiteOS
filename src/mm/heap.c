#include <mm.h>
#include <cpu.h>
#include <string.h>
#include <console.h>

/* Dedicated virtual window for the kernel heap. Backed on demand by physical
   pages from the PMM, so the heap can grow without needing contiguous RAM. */
/* Sits just above the 1 GiB direct map (0xFFFFFFFF80000000..0xFFFFFFFFC0000000)
   so heap mappings never clobber the direct-map page tables. */
#define HEAP_BASE  0xFFFFFFFFC0000000ULL
#define HEAP_LIMIT (HEAP_BASE + (256ULL << 20))   /* 256 MiB of virtual space */
#define GROW_CHUNK (2ULL << 20)                   /* 2 MiB per grow */

struct block {
    usize size;              /* payload size in bytes */
    int   free;
    struct block *next;
    struct block *prev;
};

#define HDR_SIZE ALIGN_UP(sizeof(struct block), 16)
#define BLK_PAYLOAD(b) ((void *)((u8 *)(b) + HDR_SIZE))
#define PAYLOAD_BLOCK(p) ((struct block *)((u8 *)(p) - HDR_SIZE))

static struct block *head;
static u64 heap_brk = HEAP_BASE;
static usize heap_total;
static usize heap_used;

static void block_insert(struct block *b)
{
    if (!head || b < head) {
        b->prev = NULL;
        b->next = head;
        if (head) head->prev = b;
        head = b;
        return;
    }
    struct block *cur = head;
    while (cur->next && cur->next < b) cur = cur->next;
    b->next = cur->next;
    b->prev = cur;
    if (cur->next) cur->next->prev = b;
    cur->next = b;
}

static void try_merge(struct block *b)
{
    if (b->next && b->next->free &&
        (u8 *)b + HDR_SIZE + b->size == (u8 *)b->next) {
        struct block *n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
    if (b->prev && b->prev->free &&
        (u8 *)b->prev + HDR_SIZE + b->prev->size == (u8 *)b) {
        struct block *p = b->prev;
        p->size += HDR_SIZE + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
    }
}

static int heap_grow(usize need)
{
    usize size = ALIGN_UP(MAX(need, (usize)GROW_CHUNK), PAGE_SIZE);
    if (heap_brk + size > HEAP_LIMIT) return -1;

    usize pages = size / PAGE_SIZE;
    u64 phys = pmm_alloc_pages(pages);
    if (!phys) return -1;

    if (vmm_map_range(heap_brk, phys, size, PAGE_WRITE | PAGE_NX) != 0) {
        pmm_free_pages(phys, pages);
        return -1;
    }

    struct block *b = (struct block *)heap_brk;
    b->size = size - HDR_SIZE;
    b->free = 1;
    b->next = b->prev = NULL;
    block_insert(b);
    try_merge(b);

    heap_brk += size;
    heap_total += size;
    return 0;
}

void heap_init(void)
{
    head = NULL;
    heap_brk = HEAP_BASE;
    heap_total = heap_used = 0;
    if (heap_grow(GROW_CHUNK) != 0)
        panic("heap: initial grow failed");
}

void *kmalloc(usize size)
{
    if (size == 0) return NULL;
    size = ALIGN_UP(size, 16);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b; b = b->next) {
            if (!b->free || b->size < size) continue;

            /* Split if the remainder can hold a header plus a small payload. */
            if (b->size >= size + HDR_SIZE + 32) {
                struct block *nb = (struct block *)((u8 *)b + HDR_SIZE + size);
                nb->size = b->size - size - HDR_SIZE;
                nb->free = 1;
                nb->next = b->next;
                nb->prev = b;
                if (b->next) b->next->prev = nb;
                b->next = nb;
                b->size = size;
            }
            b->free = 0;
            heap_used += b->size;
            return BLK_PAYLOAD(b);
        }
        if (heap_grow(size + HDR_SIZE) != 0) break;
    }
    return NULL;
}

void *kzalloc(usize size)
{
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    struct block *b = PAYLOAD_BLOCK(ptr);
    if (b->free) return;              /* double free */
    b->free = 1;
    heap_used -= b->size;
    try_merge(b);
}

void *krealloc(void *ptr, usize size)
{
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return NULL; }

    struct block *b = PAYLOAD_BLOCK(ptr);
    usize want = ALIGN_UP(size, 16);
    if (b->size >= want) return ptr;

    /* Try to absorb a free neighbour before moving the payload. */
    if (b->next && b->next->free && b->size + HDR_SIZE + b->next->size >= want) {
        struct block *n = b->next;
        b->size += HDR_SIZE + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        heap_used += HDR_SIZE + n->size;
        return ptr;
    }

    void *np = kmalloc(size);
    if (!np) return NULL;
    memcpy(np, ptr, b->size);
    kfree(ptr);
    return np;
}

void heap_stats(usize *total, usize *used, usize *free_bytes)
{
    if (total) *total = heap_total;
    if (used) *used = heap_used;
    if (free_bytes) *free_bytes = heap_total - heap_used;
}
