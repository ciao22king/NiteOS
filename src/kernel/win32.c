/* Windows (PE32+) personality.
 *
 * A PE image reaches the operating system only through its import table, so
 * every import is bound to a small trampoline that traps into the kernel; the
 * emulation itself runs here at CPL 0. That keeps the guest at ring 3 (a
 * Windows program is not allowed to run kernel code) while letting the kernel
 * implement the handful of KERNEL32/msvcrt entry points a console program
 * actually needs.
 *
 * The trampoline (built in win32_init) reshuffles the Windows argument
 * registers (rcx, rdx, r8, r9) into the ones the int 0x80 stub captures
 * (rdi, rsi, rdx, rcx), then traps with the function id in eax.
 *
 * The thread environment block lives at the address in GS_BASE, which is how
 * a Windows binary finds its stack limits and TLS array.
 */
#include <win32.h>
#include <process.h>
#include <console.h>
#include <cpu.h>
#include <string.h>
#include <syscall.h>

/* Two dedicated virtual regions, clear of the PIE, mmap and stack ranges. */
#define WIN32_TEB_BASE  0x00007D0000000000ULL
#define WIN32_STUB_BASE 0x00007E0000000000ULL
#define WIN32_DATA_BASE 0x00007C0000000000ULL
#define WIN32_TEB_SIZE  (2 * PAGE_SIZE)
#define WIN32_STUB_STRIDE 32

/* Layout of the shared data page. Every pointer the CRT receives must be a
   user address, so the globals it dereferences live here rather than in the
   kernel heap. */
#define D_ERRNO     0x000
#define D_FMODE     0x004
#define D_COMMODE   0x008
#define D_MB_CUR    0x00C
#define D_INITENV   0x010
#define D_P_FMODE   0x018
#define D_IOB       0x040
#define D_LCONV     0x100
#define D_LCONV_DEC (D_LCONV + 0x08)
#define D_LCONV_THO (D_LCONV + 0x10)
#define IOB_STRIDE  48

/* Offsets inside the TEB that the mingw runtime reads. */
#define TEB_STACK_BASE      0x08
#define TEB_STACK_LIMIT     0x10
#define TEB_SELF            0x30
#define TEB_PROCESS_ID      0x40
#define TEB_THREAD_ID       0x48
#define TEB_TLS_POINTER     0x58

enum {
    W32__FMODE = 1, W32__COMMODE, W32__INITENV, W32__ERRNO, W32__IOB_FUNC,
    W32__MB_CUR_MAX, W32__P__FMODE,
    W32_GET_LAST_ERROR, W32_SET_UNHANDLED, W32_SLEEP, W32_DELETE_CS,
    W32_ENTER_CS, W32_LEAVE_CS, W32_INIT_CS, W32_ISDBCS, W32_MBTOWC,
    W32_WCTOMB, W32_TLS_GET_VALUE, W32_VIRTUAL_PROTECT, W32_VIRTUAL_QUERY,
    W32_GET_MODULE_HANDLE, W32_GET_PROC_ADDRESS, W32_EXIT_PROCESS,
    W32_GET_STD_HANDLE,
    W32_GETMAINARGS, W32_SET_APP_TYPE, W32_SETUSERMATHERR, W32_C_SPECIFIC,
    W32_AMSG_EXIT, W32_CEXIT, W32_INITTERM, W32_LOCK, W32_UNLOCK, W32_ONEXIT,
    W32_ABORT, W32_CALLOC, W32_EXIT, W32_FPRINTF, W32_FPUTC, W32_FREE,
    W32_FWRITE, W32_LOCALECONV, W32_MALLOC, W32_MEMCPY, W32_MEMSET, W32_SIGNAL,
    W32_STRERROR, W32_STRLEN, W32_STRNCMP, W32_VFPRINTF, W32_WCSLEN,
    W32_ACRT_IOB_FUNC, W32_LOCK_FILE, W32_UNLOCK_FILE, W32_SET_INVALID_PARAM,
    W32_FPRESET,
    W32_LC_CODEPAGE,
    W32_UNKNOWN
};

struct win32_import {
    const char *name;
    u16         id;
};

/* Every symbol the mingw CRT imports from KERNEL32 and msvcrt. Anything not
   listed binds to a stub that returns 0, so an unexpected import degrades to a
   no-op instead of a jump through a null pointer. */
static const struct win32_import g_imports[] = {
    { "_errno",                 W32__ERRNO },
    { "__iob_func",             W32__IOB_FUNC },
    { "___mb_cur_max_func",     W32__MB_CUR_MAX },
    { "___lc_codepage_func",    W32_LC_CODEPAGE },
    { "GetLastError",           W32_GET_LAST_ERROR },
    { "SetUnhandledExceptionFilter", W32_SET_UNHANDLED },
    { "Sleep",                  W32_SLEEP },
    { "DeleteCriticalSection",  W32_DELETE_CS },
    { "EnterCriticalSection",   W32_ENTER_CS },
    { "LeaveCriticalSection",   W32_LEAVE_CS },
    { "InitializeCriticalSection", W32_INIT_CS },
    { "IsDBCSLeadByteEx",       W32_ISDBCS },
    { "MultiByteToWideChar",    W32_MBTOWC },
    { "WideCharToMultiByte",    W32_WCTOMB },
    { "TlsGetValue",            W32_TLS_GET_VALUE },
    { "VirtualProtect",         W32_VIRTUAL_PROTECT },
    { "VirtualQuery",           W32_VIRTUAL_QUERY },
    { "GetModuleHandleA",       W32_GET_MODULE_HANDLE },
    { "GetProcAddress",         W32_GET_PROC_ADDRESS },
    { "ExitProcess",            W32_EXIT_PROCESS },
    { "GetStdHandle",           W32_GET_STD_HANDLE },
    { "__getmainargs",          W32_GETMAINARGS },
    { "__set_app_type",         W32_SET_APP_TYPE },
    { "__setusermatherr",       W32_SETUSERMATHERR },
    { "__C_specific_handler",   W32_C_SPECIFIC },
    { "_amsg_exit",             W32_AMSG_EXIT },
    { "_cexit",                 W32_CEXIT },
    { "_initterm",              W32_INITTERM },
    { "_lock",                  W32_LOCK },
    { "_unlock",                W32_UNLOCK },
    { "_onexit",                W32_ONEXIT },
    { "abort",                  W32_ABORT },
    { "calloc",                 W32_CALLOC },
    { "exit",                   W32_EXIT },
    { "fprintf",                W32_FPRINTF },
    { "fputc",                  W32_FPUTC },
    { "free",                   W32_FREE },
    { "fwrite",                 W32_FWRITE },
    { "localeconv",             W32_LOCALECONV },
    { "malloc",                 W32_MALLOC },
    { "memcpy",                 W32_MEMCPY },
    { "memset",                 W32_MEMSET },
    { "signal",                 W32_SIGNAL },
    { "strerror",               W32_STRERROR },
    { "strlen",                 W32_STRLEN },
    { "strncmp",                W32_STRNCMP },
    { "vfprintf",               W32_VFPRINTF },
    { "wcslen",                 W32_WCSLEN },
    { "__acrt_iob_func",        W32_ACRT_IOB_FUNC },
    { "_lock_file",             W32_LOCK_FILE },
    { "_unlock_file",           W32_UNLOCK_FILE },
    { "_set_invalid_parameter_handler", W32_SET_INVALID_PARAM },
    { "_fpreset",               W32_FPRESET },
};

static u8  *g_teb;             /* kernel mapping of the TEB page */
static u8  *g_teb2;            /* kernel mapping of the TEB's second page (argv) */
static u8  *g_data;            /* kernel mapping of the data page */
static u64  g_teb_user;
static u64  g_stub_user;
static u32  g_stub_next;
static u64  g_argv_va, g_envp_va;

static u64 data_user(u32 off) { return WIN32_DATA_BASE + off; }

/* Zeroing has to happen per page through that page's own kernel mapping:
   consecutive virtual pages are not backed by consecutive physical ones, so a
   single memset across the range would clobber whatever page the allocator
   happened to place next (in practice, a page table). */
static void map_at(u64 va, void **kern, u64 pages)
{
    struct process *p = process_current();
    for (u64 i = 0; i < pages; i++) {
        u64 phys = pmm_alloc_page();
        if (vmm_space_map(p->space, va + i * PAGE_SIZE, phys,
                          PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page(phys);
            return;
        }
        memset((void *)phys_to_virt(phys), 0, PAGE_SIZE);
        if (i == 0 && kern)
            *kern = (void *)phys_to_virt(phys);
    }
}

/* Build one trampoline:  push rdi; push rsi; mov r11,rdx; mov rdi,rcx;
   mov rsi,r11; mov rdx,r8; mov rcx,r9; mov eax,id; int 0x80;
   pop rsi; pop rdi; ret */
static void build_stub(u8 *at, u32 id)
{
    static const u8 prefix[] = {
        0x57, 0x56,                         /* push rdi; push rsi */
        0x49, 0x89, 0xd3,                   /* mov r11, rdx */
        0x48, 0x89, 0xcf,                   /* mov rdi, rcx */
        0x4c, 0x89, 0xde,                   /* mov rsi, r11 */
        0x4c, 0x89, 0xc2,                   /* mov rdx, r8 */
        0x4c, 0x89, 0xc9,                   /* mov rcx, r9 */
    };
    static const u8 suffix[] = { 0xcd, 0x80, 0x5e, 0x5f, 0xc3 };
    memset(at, 0x90, WIN32_STUB_STRIDE);
    memcpy(at, prefix, sizeof(prefix));
    u8 *p = at + sizeof(prefix);
    *p++ = 0xb8;                            /* mov eax, imm32 */
    *(u32 *)p = id; p += 4;
    memcpy(p, suffix, sizeof(suffix));
}

void win32_init(void)
{
    map_at(WIN32_TEB_BASE, (void **)&g_teb, WIN32_TEB_SIZE / PAGE_SIZE);
    g_teb_user = WIN32_TEB_BASE;
    /* The argv area is the *second virtual* page of the TEB, which the
       allocator does not place right after the first one; resolve its own
       kernel mapping instead of assuming g_teb + PAGE_SIZE. */
    {
        struct process *pp = process_current();
        u64 p2 = pp ? vmm_space_get_phys(pp->space, WIN32_TEB_BASE + PAGE_SIZE) : 0;
        g_teb2 = p2 ? (u8 *)phys_to_virt(p2) : NULL;
    }

    map_at(WIN32_DATA_BASE, (void **)&g_data, 1);
    if (g_data) {
        *(u32 *)(g_data + D_MB_CUR) = 1;
        *(u64 *)(g_data + D_P_FMODE) = data_user(D_FMODE);
        *(u64 *)(g_data + D_LCONV_DEC) = data_user(D_LCONV + 0x40);
        *(u64 *)(g_data + D_LCONV_THO) = data_user(D_LCONV + 0x48);
        g_data[D_LCONV + 0x40] = '.';      /* decimal_point */
        g_data[D_LCONV + 0x48] = '\0';     /* thousands_sep */
    }

    /* The stub page has to be executable; write it through the direct map. */
    struct process *p = process_current();
    u64 phys = pmm_alloc_page();
    if (vmm_space_map(p->space, WIN32_STUB_BASE, phys,
                      PAGE_PRESENT | PAGE_USER) == 0)
        memset((void *)phys_to_virt(phys), 0x90, PAGE_SIZE);
    g_stub_user = WIN32_STUB_BASE;
    g_stub_next = 0;
}

/* These four are *data* imports: the CRT dereferences the IAT slot instead of
   calling through it (`mov rax,[__imp__fmode]; mov eax,[rax]`). Their IAT entry
   therefore has to hold the address of the variable itself, not a trampoline. */
static const struct { const char *name; u32 off; } g_data_imports[] = {
    { "_fmode",    D_FMODE },
    { "_commode",  D_COMMODE },
    { "__initenv", D_INITENV },
    { "_p__fmode", D_P_FMODE },
};

u64 win32_bind_import(const char *name)
{
    for (usize i = 0; i < sizeof(g_data_imports) / sizeof(g_data_imports[0]); i++)
        if (!strcmp(name, g_data_imports[i].name))
            return data_user(g_data_imports[i].off);

    u16 id = W32_UNKNOWN;
    for (usize i = 0; i < sizeof(g_imports) / sizeof(g_imports[0]); i++) {
        if (!strcmp(name, g_imports[i].name)) { id = g_imports[i].id; break; }
    }
    if ((g_stub_next + 1) * WIN32_STUB_STRIDE > PAGE_SIZE) return 0;
    u64 phys = vmm_space_get_phys(process_current()->space, g_stub_user);
    if (!phys) return 0;
    u8 *page = (u8 *)phys_to_virt(phys & PAGE_MASK);
    build_stub(page + g_stub_next * WIN32_STUB_STRIDE, id);
    u64 addr = g_stub_user + g_stub_next * WIN32_STUB_STRIDE;
    g_stub_next++;
    return addr;
}

void win32_set_tls(void)
{
    struct process *p = process_current();
    if (!g_teb || !p) return;
    memset(g_teb, 0, PAGE_SIZE);
    if (g_teb2) memset(g_teb2, 0, PAGE_SIZE);
    *(u64 *)(g_teb + TEB_STACK_BASE)  = p->user_stack_top;
    *(u64 *)(g_teb + TEB_STACK_LIMIT) = p->user_stack_top - (8 * 1024 * 1024);
    *(u64 *)(g_teb + TEB_SELF)        = g_teb_user;
    *(u32 *)(g_teb + TEB_PROCESS_ID)  = (u32)p->pid;
    *(u32 *)(g_teb + TEB_THREAD_ID)   = (u32)p->pid;
    *(u64 *)(g_teb + TEB_TLS_POINTER) = 0;
    /* The guest reads gs:0x30 etc., so its GS base must be the TEB. The
       kernel keeps a zero base of its own; syscall_stub swaps between them. */
    wrmsr(MSR_GS_BASE, g_teb_user);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

/* ---------------- the emulated API surface ---------------- */

static void out(const char *s, usize n) { for (usize i = 0; i < n; i++) console_putc(s[i]); }

/* A minimal printf so an imported fprintf/vfprintf still produces output. Only
   the first two variadic arguments can be recovered (the stub captures four
   registers and the rest live on the guest stack), which covers the format
   strings a simple console program uses. */
static void win32_format(const char *fmt, u64 a1, u64 a2)
{
    int next = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { console_putc(*p); continue; }
        p++;
        if (*p == '%') { console_putc('%'); continue; }
        int width = 0, zero = 0;
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        while (*p == 'l' || *p == 'z' || *p == 'h') p++;
        u64 v = next++ == 0 ? a1 : a2;
        char buf[24];
        int n = 0;
        if (*p == 's') {
            const char *s = (const char *)v;
            out(s ? s : "(null)", s ? strlen(s) : 6);
            continue;
        }
        if (*p == 'c') { console_putc((char)v); continue; }
        unsigned base = 10;
        const char *digits = "0123456789abcdef";
        if (*p == 'x' || *p == 'X' || *p == 'p') base = 16;
        unsigned long long uv = (unsigned long long)v;
        if (uv == 0) buf[n++] = '0';
        while (uv) { buf[n++] = digits[uv % base]; uv /= base; }
        while (n < width) buf[n++] = zero ? '0' : ' ';
        while (n) console_putc(buf[--n]);
    }
}

/* The argv/envp block __getmainargs hands back. It is built once per process
   in the second TEB page and stays valid for the life of the program. */
static u8 *win32_argv_area(void) { return g_teb2; }

static void build_argv(void)
{
    struct process *p = process_current();
    u8 *area = win32_argv_area();
    u64 *argv = (u64 *)area;
    char *strs = (char *)(area + 0x400);
    u64 *envp = (u64 *)(area + 0x800);
    int n = p->argc > 0 ? p->argc : 1;
    for (int i = 0; i < n; i++) {
        const char *src = (i < p->argc && p->argv[i][0]) ? p->argv[i] : p->name;
        usize l = strlen(src) + 1;
        memcpy(strs, src, l);
        argv[i] = (u64)(WIN32_TEB_BASE + PAGE_SIZE + ((u8 *)strs - area));
        strs += l;
    }
    argv[n] = 0;
    envp[0] = 0;
    g_argv_va = WIN32_TEB_BASE + PAGE_SIZE;
    g_envp_va = WIN32_TEB_BASE + PAGE_SIZE + 0x800;
}

u64 win32_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3)
{
    switch ((u32)nr) {
    case W32_UNKNOWN:
    case W32_GET_LAST_ERROR:
    case W32_SET_UNHANDLED:
    case W32_DELETE_CS:
    case W32_ENTER_CS:
    case W32_LEAVE_CS:
    case W32_INIT_CS:
    case W32_ISDBCS:
    case W32_TLS_GET_VALUE:
    case W32_VIRTUAL_QUERY:
    case W32_GET_MODULE_HANDLE:
    case W32_GET_PROC_ADDRESS:
    case W32_GET_STD_HANDLE:
    case W32_SETUSERMATHERR:
    case W32_C_SPECIFIC:
    case W32_LOCK:
    case W32_UNLOCK:
    case W32_ONEXIT:
    case W32_SIGNAL:
    case W32_LOCK_FILE:
    case W32_UNLOCK_FILE:
    case W32_SET_INVALID_PARAM:
    case W32_FPRESET:
    case W32_SET_APP_TYPE:
        return 0;

    case W32_SLEEP: {
        u64 until = irq_get_ticks() + (a0 / 10) + 1;
        while (irq_get_ticks() < until) __asm__ volatile("hlt");
        return 0;
    }

    case W32__FMODE:      return data_user(D_FMODE);
    case W32__COMMODE:    return data_user(D_COMMODE);
    case W32__MB_CUR_MAX: return 1;   /* ___mb_cur_max_func() returns the value */
    case W32__ERRNO:      return data_user(D_ERRNO);
    case W32__P__FMODE:   return data_user(D_FMODE);
    case W32__IOB_FUNC:
    case W32_ACRT_IOB_FUNC: return data_user(D_IOB);
    case W32__INITENV:
        build_argv();
        *(u64 *)(g_data + D_INITENV) = g_envp_va;
        return data_user(D_INITENV);

    case W32_GETMAINARGS: {
        /* int __getmainargs(int *argc, char ***argv, char ***envp,
                            int expand, void *startinfo) */
        build_argv();
        *(int *)a0 = process_current()->argc;
        *(u64 *)a1 = g_argv_va;
        *(u64 *)a2 = g_envp_va;
        return 0;
    }

    case W32_INITTERM: {
        /* _initterm(first, last): call every non-null initialiser. */
        u64 *it = (u64 *)a0;
        u64 *end = (u64 *)a1;
        while (it < end) { if (*it) ((void (*)(void))*it)(); it++; }
        return 0;
    }

    case W32_VIRTUAL_PROTECT: return 1;   /* memory is already RWX-capable */
    case W32_MBTOWC:          return 0;
    case W32_WCTOMB:          return (u64)strlen((const char *)a0);

    case W32_MALLOC:  return process_mmap(0, a0 ? a0 : 1, 3, 0);
    case W32_CALLOC: {
        u64 total = (a0 ? a0 : 1) * (a1 ? a1 : 1);
        u64 p = process_mmap(0, total, 3, 0);
        if (p != (u64)-1) memset((void *)(u64)p, 0, total);
        return p;
    }
    case W32_FREE:    return 0;

    case W32_MEMCPY:  return (u64)(u64)memcpy((void *)(u64)a0, (const void *)(u64)a1, a2);
    case W32_MEMSET:  return (u64)(u64)memset((void *)(u64)a0, (int)a1, a2);
    case W32_STRLEN:  return (u64)strlen((const char *)a0);
    case W32_WCSLEN: {
        const u16 *w = (const u16 *)a0; u64 n = 0;
        while (w && w[n]) n++;
        return n;
    }
    case W32_STRNCMP: return (u64)(i64)strncmp((const char *)a0, (const char *)a1, a2);
    case W32_STRERROR: return (u64)(u64)"error";
    case W32_LOCALECONV: return data_user(D_LCONV);

    case W32_FPUTC:   console_putc((char)a0); return (u64)(u8)a0;
    case W32_FWRITE: {
        usize total = a1 * a2;
        out((const char *)a0, total);
        return a2;
    }
    case W32_FPRINTF:
    case W32_VFPRINTF:
        /* fprintf(FILE*, fmt, ...) / vfprintf(FILE*, fmt, va_list) */
        win32_format((const char *)a1, a2, a3);
        return 0;

    case W32_CEXIT:   return 0;
    case W32_AMSG_EXIT:
        console_printf("win32: runtime abort (%d)\\n", (int)a0);
        process_exit(255);
    case W32_ABORT:   console_write("win32: abort()\\n"); process_exit(134);
    case W32_EXIT:
    case W32_EXIT_PROCESS:
        process_exit((int)a0);
    }
    return 0;
}
