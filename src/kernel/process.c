#include <process.h>
#include <syscall.h>
#include <console.h>
#include <string.h>
#include <cpu.h>
#include <neofs.h>
#include <devices.h>
#include <loader.h>

/* Not marked noreturn on purpose: GCC would turn the call into a `jmp`, and
   user_ctx_save captures its own return address to unwind back here when the
   process exits. It is declared as an ordinary function that "returns" via
   user_ctx_restore. */
extern void user_ctx_save(u64 entry, u64 user_rsp, u64 cr3, u64 kstack_top);
extern void user_ctx_restore(void) __attribute__((noreturn));

#define USER_STACK  0x00007FFFFFF00000ULL   /* top of the user stack */
#define USER_STACK_SIZE (8 * 1024 * 1024)   /* glibc wants megabytes, not 64 KiB */
#define USER_HEAP_BASE 0x0000000008000000ULL
#define USER_HEAP_MAX  (256 * 1024 * 1024)

static struct process procs[1];
static struct process *current;
static int next_pid = 1;

void process_init(void)
{
    memset(procs, 0, sizeof(procs));
    current = NULL;
    next_pid = 1;
}

struct process *process_current(void) { return current; }
int process_current_pid(void) { return current ? current->pid : 0; }

static struct process *proc_alloc(void)
{
    for (usize i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
        if (!procs[i].running && procs[i].space == NULL) {
            memset(&procs[i], 0, sizeof(procs[i]));
            procs[i].pid = next_pid++;
            procs[i].running = 1;
            return &procs[i];
        }
    }
    return NULL;
}

static void proc_free(struct process *p)
{
    if (!p) return;
    if (p->space) vmm_free_space(p->space);
    p->space = NULL;
    p->running = 0;
    if (current == p) current = NULL;
}

/* Copy `len` bytes into the user stack at `vaddr`, resolving each page as it
   goes. The stack pages are not physically contiguous, so a single memcpy
   through the direct map would be wrong. */
static int stack_write(struct process *p, u64 vaddr, const void *src, u32 len)
{
    const u8 *s = (const u8 *)src;
    u32 done = 0;
    while (done < len) {
        u64 va = vaddr + done;
        u64 page = ALIGN_DOWN(va, PAGE_SIZE);
        u32 off = (u32)(va - page);
        u32 chunk = (u32)(PAGE_SIZE - off);
        if (chunk > len - done) chunk = len - done;
        u64 phys = vmm_space_get_phys(p->space, page);
        if (!phys) return -1;
        memcpy((u8 *)phys_to_virt(phys & PTE_ADDR_MASK) + off, s + done, chunk);
        done += chunk;
    }
    return 0;
}

/* Map the user stack and copy argv/env onto it. Returns the new stack pointer. */
static u64 setup_user_stack(struct process *p)
{
    u64 stack_base = USER_STACK - USER_STACK_SIZE;
    usize pages = USER_STACK_SIZE / PAGE_SIZE;

    /* Page-at-a-time: an 8 MiB contiguous physical run is not guaranteed, and
       the stack must not depend on one. Writes go through a per-page copy that
       re-walks the mapping, so the backing need not be contiguous either. */
    for (usize i = 0; i < pages; i++) {
        u64 phys = pmm_alloc_page();
        if (!phys) return 0;
        if (vmm_space_map(p->space, stack_base + i * PAGE_SIZE, phys,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page(phys);
            return 0;
        }
    }

    #define STACK_AT(v) ((u8 *)(v))

    u64 sp = USER_STACK;
    u64 arg_ptrs[PROC_MAX_ARGS + 1];
    u64 env_ptrs[PROC_MAX_ENV + 1];

    #define PUSH_STR(str) do {                                \
        u32 l = (u32)strlen(str) + 1;                         \
        sp -= l;                                              \
        stack_write(p, sp, (str), l);                         \
    } while (0)

    for (int i = p->envc - 1; i >= 0; i--) { PUSH_STR(p->env[i]); env_ptrs[i] = sp; }
    for (int i = p->argc - 1; i >= 0; i--) { PUSH_STR(p->argv[i]); arg_ptrs[i] = sp; }
    #undef PUSH_STR

    sp &= ~0xFULL;

    /* The kernel hands the stack over in this order, from low to high address:
       argc, argv[], NULL, envp[], NULL, auxv pairs, AT_NULL. The stack grows
       down, so the highest-address item is written first and argc last. */
    /* 16 random bytes for AT_RANDOM. glibc derives its stack canary and
       pointer guard from these; without them it dereferences NULL. */
    u8 rnd[16];
    process_getrandom(rnd, sizeof(rnd));
    sp -= sizeof(rnd); stack_write(p, sp, rnd, sizeof(rnd));
    u64 at_random = sp;

    const char plat[] = "x86_64";
    sp -= sizeof(plat); stack_write(p, sp, plat, sizeof(plat));
    u64 at_platform = sp;

    u32 nlen = (u32)strlen(p->name) + 1;
    sp -= nlen; stack_write(p, sp, p->name, nlen);
    u64 at_execfn = sp;

    u64 aux[][2] = {
        { 6,  PAGE_SIZE },                 /* AT_PAGESZ     */
        { 9,  p->entry },                  /* AT_ENTRY      */
        { 3,  p->phdr },                   /* AT_PHDR       */
        { 4,  sizeof(struct elf64_phdr) }, /* AT_PHENT      */
        { 5,  p->phnum },                  /* AT_PHNUM      */
        { 25, at_random },                 /* AT_RANDOM     */
        { 15, at_platform },               /* AT_PLATFORM   */
        { 31, at_execfn },                 /* AT_EXECFN     */
        { 16, 0 },                         /* AT_HWCAP      */
        { 23, 0 },                         /* AT_SECURE     */
        { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 }, /* uid/euid/gid/egid */
        { 17, 100 },                       /* AT_CLKTCK     */
        { 33, 0 },                         /* AT_SYSINFO_EHDR: no vDSO */
        { 0,  0 },                         /* AT_NULL       */
    };
    for (int i = (int)(sizeof(aux) / sizeof(aux[0])) - 1; i >= 0; i--) {
        sp -= 8; stack_write(p, sp, &aux[i][1], 8);
        sp -= 8; stack_write(p, sp, &aux[i][0], 8);
    }

    sp -= 8; stack_write(p, sp, &(u64){0}, 8);         /* envp terminator */
    for (int i = p->envc - 1; i >= 0; i--) {
        sp -= 8; stack_write(p, sp, &env_ptrs[i], 8);
    }
    sp -= 8; stack_write(p, sp, &(u64){0}, 8);         /* argv terminator */
    for (int i = p->argc - 1; i >= 0; i--) {
        sp -= 8; stack_write(p, sp, &arg_ptrs[i], 8);
    }
    u64 argc64 = (u64)p->argc;
    sp -= 8; stack_write(p, sp, &argc64, 8);           /* argc */

    #undef STACK_AT
    return sp;
}

/* Demand paging for the user stack. glibc and large programs grow the stack
   well past what we pre-map, so a not-present fault below the stack top is
   satisfied here instead of panicking. Returns 0 if the fault was handled. */
#define USER_STACK_GROW_LIMIT (USER_STACK - (64 * 1024 * 1024))

int process_handle_fault(u64 cr2)
{
    struct process *p = current;
    if (!p || !p->space) return -1;
    if (cr2 >= USER_STACK || cr2 < USER_STACK_GROW_LIMIT) return -1;

    u64 page = ALIGN_DOWN(cr2, PAGE_SIZE);
    u64 phys = pmm_alloc_page();
    if (!phys) return -1;
    if (vmm_space_map(p->space, page, phys,
                      PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
        pmm_free_page(phys);
        return -1;
    }
    return 0;
}

/* Address range reserved for ET_DYN (PIE / AppImage runtime) images. Kept away
   from the brk heap and the mmap region so they cannot collide. */
#define USER_PIE_BASE 0x0000200000000000ULL
#define USER_PIE_END  0x0000300000000000ULL
static u64 pie_hint = USER_PIE_BASE;

/* Choose where a PIE image's lowest virtual address should land. The caller
   derives the load bias as (returned base - image vbase). */
u64 process_reserve_image(struct process *p, u64 vbase, u64 span)
{
    (void)p; (void)vbase;
    u64 size = ALIGN_UP(span ? span : PAGE_SIZE, PAGE_SIZE);
    u64 base = ALIGN_UP(pie_hint, PAGE_SIZE);
    if (base + size > USER_PIE_END) return 0;
    pie_hint = base + size + PAGE_SIZE;      /* leave a guard page */
    return base;
}

/* Anonymous mmap. Only MAP_ANONYMOUS is honoured; a file-backed mapping has no
   pager here, so it is refused rather than silently returning zeroed pages. */
#define USER_MMAP_BASE 0x0000400000000000ULL
#define USER_MMAP_END  0x0000700000000000ULL
static u64 mmap_hint = USER_MMAP_BASE;

u64 process_mmap(u64 addr, u64 len, u64 prot, int flags)
{
    struct process *p = current;
    if (!p || !p->space || len == 0) return (u64)-1;
    len = ALIGN_UP(len, PAGE_SIZE);

    if (!(addr) || addr < USER_MMAP_BASE || addr >= USER_MMAP_END)
        addr = mmap_hint;
    if (addr + len > USER_MMAP_END) return (u64)-1;

    u64 vflags = PAGE_PRESENT | PAGE_USER | ((prot & 2) ? PAGE_WRITE : 0);
    if (vmm_space_alloc(p->space, addr, len, vflags) != 0) return (u64)-1;
    mmap_hint = addr + len;
    return addr;
}

/* Change protection on an already-mapped range. glibc reserves memory with
   PROT_NONE and then mprotects it writable, so a no-op here leaves every such
   page read-only and the first store faults. */
int process_mprotect(u64 addr, u64 len, u64 prot)
{
    struct process *p = current;
    if (!p || !p->space) return -1;
    if (len == 0) return 0;
    addr = ALIGN_DOWN(addr, PAGE_SIZE);
    u64 end = ALIGN_UP(addr + len, PAGE_SIZE);

    for (u64 va = addr; va < end; va += PAGE_SIZE) {
        u64 phys = vmm_space_get_phys(p->space, va);
        if (!phys) continue;              /* nothing mapped: nothing to change */
        u64 flags = PAGE_PRESENT | PAGE_USER;
        if (prot & 2) flags |= PAGE_WRITE;   /* PROT_WRITE */
        if (!(prot & 4)) flags |= PAGE_NX;   /* not PROT_EXEC */
        if (vmm_space_map(p->space, va, phys & PTE_ADDR_MASK, flags) != 0)
            return -1;
    }
    return 0;
}

/* arch_prctl(ARCH_SET_FS) is how glibc installs its thread pointer. */
int process_arch_prctl(int code, u64 addr)
{
    if (code == 0x1002) {          /* ARCH_SET_FS */
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    }
    if (code == 0x1001) {          /* ARCH_GET_FS */
        return 0;
    }
    return -1;
}

u64 process_getrandom(void *buf, u32 len)
{
    if (!buf) return (u64)-1;
    /* A real CSPRNG is out of scope; a xorshift seeded from the TSC keeps
       glibc's stack canary and hash seeds from being constant. */
    static u64 state;
    if (!state) {
        u32 lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        state = ((u64)hi << 32) | lo;
        if (!state) state = 0x9E3779B97F4A7C15ULL;
    }
    u8 *out = (u8 *)buf;
    for (u32 i = 0; i < len; i++) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        out[i] = (u8)(state >> 24);
    }
    return len;
}

isize process_sbrk(isize increment)
{
    struct process *p = current;
    if (!p) return -1;
    if (p->brk_cur == 0) {
        p->brk_base = USER_HEAP_BASE;
        p->brk_cur = USER_HEAP_BASE;
        p->brk_limit = USER_HEAP_BASE + USER_HEAP_MAX;
    }
    u64 want = (u64)((isize)p->brk_cur + increment);
    if (want < p->brk_base || want > p->brk_limit) return -1;

    u64 old_end = ALIGN_UP(p->brk_cur, PAGE_SIZE);
    u64 new_end = ALIGN_UP(want, PAGE_SIZE);
    if (new_end > old_end) {
        if (vmm_space_alloc(p->space, old_end, new_end - old_end,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0)
            return -1;
    }
    isize ret = (isize)p->brk_cur;
    p->brk_cur = want;
    return ret;
}

u64 process_write_args(char *buf, u32 len)
{
    struct process *p = current;
    if (!p || !buf || !len) return (u64)-1;
    u32 pos = 0;
    for (int i = 0; i < p->argc && pos + 1 < len; i++) {
        u32 l = (u32)strlen(p->argv[i]);
        if (pos + l + 2 >= len) break;
        memcpy(buf + pos, p->argv[i], l);
        pos += l;
        buf[pos++] = (i + 1 < p->argc) ? ' ' : '\n';
    }
    buf[pos] = '\0';
    return pos;
}

void process_exit(int code)
{
    struct process *p = current;
    if (p) p->exit_code = code;
    vmm_switch_space(vmm_kernel_space());
    syscall_set_abi(NEO_ABI_NATIVE);
    if (p) {
        struct addr_space *dead = p->space;
        p->space = NULL;
        p->running = 0;
        current = NULL;
        if (dead) vmm_free_space(dead);
    }
    user_ctx_restore();
    __builtin_unreachable();
}

/* Enter ring 3 for the current process and block until it exits. */
static int enter_and_wait(struct process *p)
{
    u64 sp = setup_user_stack(p);
    if (!sp) {
        klog_err("process: could not build a user stack for %s", p->name);
        proc_free(p);
        return -1;
    }
    p->user_stack_top = sp;

    syscall_set_abi(p->kind == PROC_KIND_LINUX ? NEO_ABI_LINUX :
                    p->kind == PROC_KIND_WIN32 ? NEO_ABI_WIN32 : NEO_ABI_NATIVE);
    syscall_set_cwd(p->cwd);
    syscall_reset_fds();

    /* Per-process kernel stack. It must live in the kernel half so it stays
       mapped after CR3 switches to the process address space; it is also what
       TSS.rsp0 points at when int 0x80 traps in from ring 3. */
    static u8 kstack[16 * 1024] __attribute__((aligned(16)));
    u64 kstack_top = (u64)kstack + sizeof(kstack);
    tss_set_kernel_stack(kstack_top);
    /* The `syscall` instruction does not switch stacks by itself. */
    syscall_set_kernel_stack(kstack_top);

    user_ctx_save(p->entry, sp, vmm_space_cr3(p->space), kstack_top);

    int code = p->exit_code;
    proc_free(p);
    return code;
}

int process_exec_image(const char *name, const u8 *image, u32 size,
                       int argc, char **argv)
{
    struct process *p = proc_alloc();
    if (!p) { klog_err("process: table full"); return -1; }

    strlcpy(p->name, name ? name : "(anon)", sizeof(p->name));
    strlcpy(p->cwd, "/", sizeof(p->cwd));
    p->argc = argc < PROC_MAX_ARGS ? argc : PROC_MAX_ARGS;
    for (int i = 0; i < p->argc; i++)
        strlcpy(p->argv[i], argv[i] ? argv[i] : "", sizeof(p->argv[i]));

    p->space = vmm_new_space();
    if (!p->space) { proc_free(p); return -1; }

    current = p;

    /* The format layer picks a loader based on the image magic. */
    int rc = loader_load(p, image, size);
    if (rc != 0) {
        klog_err("process: no loader accepted %s", p->name);
        proc_free(p);
        return -1;
    }
    return enter_and_wait(p);
}

int process_run_file(const char *path, int kind, int argc, char **argv)
{
    (void)kind;
    long fsize = neofs_file_size(path);
    if (fsize < 0) {
        klog_err("process: %s not found", path);
        return -1;
    }
    u32 size = (u32)fsize;
    u8 *image = kmalloc(size ? size : 1);
    if (!image) return -1;
    if (neofs_read_file(path, image, size, &size) != 0) { kfree(image); return -1; }

    int rc = process_exec_image(path, image, size, argc, argv);
    kfree(image);
    return rc;
}
