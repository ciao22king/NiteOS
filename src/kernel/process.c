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
#define USER_STACK_SIZE (64 * 1024)
#define USER_HEAP_BASE 0x0000000008000000ULL
#define USER_HEAP_MAX  (16 * 1024 * 1024)

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

/* Map the user stack and copy argv/env onto it. Returns the new stack pointer. */
static u64 setup_user_stack(struct process *p)
{
    u64 stack_base = USER_STACK - USER_STACK_SIZE;
    usize pages = USER_STACK_SIZE / PAGE_SIZE;

    /* Contiguous physical backing so the kernel can write it through the
       direct map without re-walking the page tables for every push. */
    u64 phys = pmm_alloc_pages(pages);
    if (!phys) return 0;
    for (usize i = 0; i < pages; i++)
        if (vmm_space_map(p->space, stack_base + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0)
            return 0;

    u8 *stack = (u8 *)phys_to_virt(phys);
    #define STACK_AT(v) (stack + ((v) - stack_base))

    u64 sp = USER_STACK;
    u64 arg_ptrs[PROC_MAX_ARGS + 1];
    u64 env_ptrs[PROC_MAX_ENV + 1];

    #define PUSH_STR(s) do {                                  \
        u32 l = (u32)strlen(s) + 1;                           \
        sp -= l;                                              \
        memcpy(STACK_AT(sp), (s), l);                         \
    } while (0)

    for (int i = p->envc - 1; i >= 0; i--) { PUSH_STR(p->env[i]); env_ptrs[i] = sp; }
    for (int i = p->argc - 1; i >= 0; i--) { PUSH_STR(p->argv[i]); arg_ptrs[i] = sp; }
    #undef PUSH_STR

    sp &= ~0xFULL;
    sp -= 8; *(u64 *)STACK_AT(sp) = 0;                 /* envp terminator */
    for (int i = p->envc - 1; i >= 0; i--) { sp -= 8; *(u64 *)STACK_AT(sp) = env_ptrs[i]; }
    sp -= 8; *(u64 *)STACK_AT(sp) = 0;                 /* argv terminator */
    for (int i = p->argc - 1; i >= 0; i--) { sp -= 8; *(u64 *)STACK_AT(sp) = arg_ptrs[i]; }
    sp -= 8; *(u64 *)STACK_AT(sp) = (u64)p->argc;      /* argc */
    #undef STACK_AT
    return sp;
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
