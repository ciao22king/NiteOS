#include <mm.h>
#include <string.h>
#include <console.h>

static u64 *pml4;
static usize mapped_pages;

/* The kernel occupies the top half (indices 256..511); every user space shares
   those PML4 entries verbatim so interrupts and syscalls keep working. */
#define KERNEL_PML4_START 256

struct addr_space {
    u64 *pml4;
    u64  pml4_phys;
};

static struct addr_space kernel_space;

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

static inline u64 *table_at(u64 phys)
{
    return (u64 *)phys_to_virt(phys & PTE_ADDR_MASK);
}

static u64 *alloc_table(void)
{
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    u64 *t = table_at(phys);
    memset(t, 0, PAGE_SIZE);
    return t;
}

/* Walk `l4` to the page-table level, creating intermediate tables as needed. */
static u64 *walk_in(u64 *l4, u64 virt, int create)
{
    u64 *pdpt = NULL, *pd = NULL, *pt = NULL;

    if (!(l4[PML4_IDX(virt)] & PAGE_PRESENT)) {
        if (!create) return NULL;
        u64 t = (u64)alloc_table();
        if (!t) return NULL;
        /* Every level of the walk must carry PAGE_USER, or a ring-3 access
           faults even when the leaf is user-accessible. */
        l4[PML4_IDX(virt)] = virt_to_phys(t) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    pdpt = table_at(l4[PML4_IDX(virt)] & PTE_ADDR_MASK);

    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) {
        if (!create) return NULL;
        u64 t = (u64)alloc_table();
        if (!t) return NULL;
        pdpt[PDPT_IDX(virt)] = virt_to_phys(t) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    pd = table_at(pdpt[PDPT_IDX(virt)] & PTE_ADDR_MASK);

    if (pd[PD_IDX(virt)] & PAGE_HUGE) return NULL;   /* 2 MiB page in the way */

    if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) {
        if (!create) return NULL;
        u64 t = (u64)alloc_table();
        if (!t) return NULL;
        pd[PD_IDX(virt)] = virt_to_phys(t) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }
    pt = table_at(pd[PD_IDX(virt)] & PTE_ADDR_MASK);
    return &pt[PT_IDX(virt)];
}

static u64 *walk(u64 virt, int create) { return walk_in(pml4, virt, create); }

void vmm_init(void)
{
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    pml4 = table_at(cr3);
    mapped_pages = 0;
    kernel_space.pml4 = pml4;
    kernel_space.pml4_phys = cr3 & PTE_ADDR_MASK;
}

int vmm_map(u64 virt, u64 phys, u64 flags)
{
    u64 *pte = walk(virt, 1);
    if (!pte) return -1;
    if (!(*pte & PAGE_PRESENT)) mapped_pages++;
    *pte = (phys & PTE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

int vmm_map_range(u64 virt, u64 phys, u64 len, u64 flags)
{
    for (u64 off = 0; off < len; off += PAGE_SIZE)
        if (vmm_map(virt + off, phys + off, flags) != 0)
            return -1;
    return 0;
}

int vmm_unmap(u64 virt)
{
    u64 *pte = walk(virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;
    *pte = 0;
    mapped_pages--;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

u64 vmm_get_phys(u64 virt)
{
    u64 *pte = walk(virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (*pte & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

void vmm_stats(usize *out) { if (out) *out = mapped_pages; }

/* ---------------- Per-process address spaces ---------------- */

struct addr_space *vmm_kernel_space(void) { return &kernel_space; }

struct addr_space *vmm_new_space(void)
{
    u64 phys = pmm_alloc_page();
    if (!phys) return NULL;
    u64 *l4 = table_at(phys);
    memset(l4, 0, PAGE_SIZE);

    for (int i = KERNEL_PML4_START; i < 512; i++)
        l4[i] = pml4[i];

    struct addr_space *as = kmalloc(sizeof(*as));
    if (!as) { pmm_free_page(phys); return NULL; }
    as->pml4 = l4;
    as->pml4_phys = phys;
    return as;
}

void vmm_free_space(struct addr_space *as)
{
    if (!as || as == &kernel_space) return;
    /* Release the user half only; the kernel half is shared. */
    for (int i = 0; i < KERNEL_PML4_START; i++) {
        if (!(as->pml4[i] & PAGE_PRESENT)) continue;
        u64 *pdpt = table_at(as->pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PAGE_PRESENT)) continue;
            u64 *pd = table_at(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PAGE_PRESENT)) continue;
                if (pd[k] & PAGE_HUGE) { pmm_free_page(pd[k] & PTE_ADDR_MASK); continue; }
                u64 *pt = table_at(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++)
                    if (pt[l] & PAGE_PRESENT) pmm_free_page(pt[l] & PTE_ADDR_MASK);
                pmm_free_page(virt_to_phys((u64)pt));
            }
            pmm_free_page(virt_to_phys((u64)pd));
        }
        pmm_free_page(virt_to_phys((u64)pdpt));
    }
    pmm_free_page(as->pml4_phys);
    kfree(as);
}

void vmm_switch_space(struct addr_space *as)
{
    if (!as) as = &kernel_space;
    __asm__ volatile("mov %0, %%cr3" ::"r"(as->pml4_phys) : "memory");
}

int vmm_space_map(struct addr_space *as, u64 virt, u64 phys, u64 flags)
{
    u64 *pte = walk_in(as->pml4, virt, 1);
    if (!pte) return -1;
    *pte = (phys & PTE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

int vmm_space_unmap(struct addr_space *as, u64 virt)
{
    u64 *pte = walk_in(as->pml4, virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;
    pmm_free_page(*pte & PTE_ADDR_MASK);
    *pte = 0;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

/* Map `len` bytes of fresh zeroed physical memory at `virt`. */
int vmm_space_alloc(struct addr_space *as, u64 virt, u64 len, u64 flags)
{
    len = ALIGN_UP(len, PAGE_SIZE);
    for (u64 off = 0; off < len; off += PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) return -1;
        if (vmm_space_map(as, virt + off, phys, flags) != 0) {
            pmm_free_page(phys);
            return -1;
        }
    }
    return 0;
}

u64 vmm_space_cr3(struct addr_space *as)
{
    return as ? as->pml4_phys : kernel_space.pml4_phys;
}

u64 vmm_space_get_phys(struct addr_space *as, u64 virt)
{
    u64 *pte = walk_in(as->pml4, virt, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (*pte & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}
