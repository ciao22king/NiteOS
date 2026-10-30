#include <syscall.h>
#include <console.h>
#include <string.h>
#include <mm.h>
#include <cpu.h>
#include <devices.h>
#include <neofs.h>
#include <process.h>

/* Syscall layer. Ring-3 programs reach the kernel through int 0x80; the stub
   in src/arch/usermode.asm hands us the register frame. */

extern void syscall_stub(void);
extern void syscall_entry(void);
extern u64  syscall_kstack_top;

static int  cur_abi = NEO_ABI_NATIVE;
static char cur_cwd[128] = "/";

void syscall_msr_init(void)
{
    /* EFER.SCE enables the `syscall` instruction. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* SYSRET computes CS = STAR[63:48]+16 and SS = STAR[63:48]+8 (both with
       RPL 3). With the base at 0x10 that yields CS 0x23 and SS 0x1b, which are
       exactly the user code and data selectors in the GDT. */
    wrmsr(MSR_STAR, ((u64)0x08 << 32) | ((u64)0x10 << 48));
    wrmsr(MSR_LSTAR, (u64)syscall_entry);
    /* Clear IF, TF and DF on entry, like Linux. */
    wrmsr(MSR_FMASK, 0x700);
}

void syscall_init(void)
{
    idt_set_gate(0x80, (u64)syscall_stub, 3);   /* DPL 3: callable from ring 3 */
    syscall_msr_init();
}

/* The kernel stack the `syscall` entry switches to. It must be set before any
   ring-3 code can execute, because RSP is still the user stack on entry. */
void syscall_set_kernel_stack(u64 top) { syscall_kstack_top = top; }

void syscall_set_abi(int abi) { cur_abi = abi; }
int  syscall_get_abi(void) { return cur_abi; }

void syscall_set_cwd(const char *dir)
{
    if (dir) strlcpy(cur_cwd, dir, sizeof(cur_cwd));
}

const char *syscall_cwd(void) { return cur_cwd; }

/* ---------------- fd table ---------------- */

#define FD_MAX 16

struct fd_entry {
    int   used;
    int   is_pipe;
    u8   *data;
    u32   size;
    u32   pos;
};

static struct fd_entry fds[FD_MAX];

static int fd_alloc(void)
{
    for (int i = 3; i < FD_MAX; i++)
        if (!fds[i].used) { memset(&fds[i], 0, sizeof(fds[i])); fds[i].used = 1; return i; }
    return -1;
}

void syscall_reset_fds(void)
{
    memset(fds, 0, sizeof(fds));
    /* 0,1,2 are the console and are always live. */
    fds[0].used = fds[1].used = fds[2].used = 1;
}

void syscall_set_stdio(const u8 *in, u32 in_len, u8 *out, u32 out_cap, u32 *out_len)
{
    syscall_reset_fds();
    fds[0].data = (u8 *)in;
    fds[0].size = in_len;
    fds[1].data = out;
    fds[1].size = out_cap;
    fds[1].pos = 0;
    fds[2].data = out;
    fds[2].size = out_cap;
    fds[2].pos = 0;
    (void)out_len;
}

u32 syscall_out_length(void) { return fds[1].pos; }

/* ---------------- handlers ---------------- */

static u64 do_write(int fd, const void *buf, u32 len)
{
    if (!buf) return (u64)-1;
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        if (fds[fd].data) {
            /* Captured into a buffer (used when the shell redirects). */
            u32 room = fds[fd].size - fds[fd].pos;
            u32 n = len < room ? len : room;
            memcpy(fds[fd].data + fds[fd].pos, p, n);
            fds[fd].pos += n;
            if (n < len) return n;
        } else {
            for (u32 i = 0; i < len; i++) console_putc(p[i]);
        }
        return len;
    }
    return (u64)-1;
}

static u64 do_read(int fd, void *buf, u32 len)
{
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used) return (u64)-1;
    if (fd == 0) {
        if (fds[0].data) {
            u32 avail = fds[0].size - fds[0].pos;
            u32 n = len < avail ? len : avail;
            memcpy(buf, fds[0].data + fds[0].pos, n);
            fds[0].pos += n;
            return n;
        }
        /* Interactive console: return one line. */
        char *out = (char *)buf;
        u32 n = 0;
        while (n + 1 < len) {
            int c = console_getchar();
            if (c < 0) { hlt(); continue; }
            if (c == '\n') { out[n++] = '\n'; break; }
            out[n++] = (char)c;
        }
        return n;
    }
    if (!fds[fd].data) return (u64)-1;
    u32 avail = fds[fd].size - fds[fd].pos;
    u32 n = len < avail ? len : avail;
    memcpy(buf, fds[fd].data + fds[fd].pos, n);
    fds[fd].pos += n;
    return n;
}

static u64 do_open(const char *path, u64 flags)
{
    (void)flags;
    if (!path) return (u64)-1;
    long fsize = neofs_file_size(path);
    if (fsize < 0) return (u64)-1;
    int fd = fd_alloc();
    if (fd < 0) return (u64)-1;
    u32 size = (u32)fsize;
    u8 *buf = kmalloc(size ? size : 1);
    if (!buf) { fds[fd].used = 0; return (u64)-1; }
    if (neofs_read_file(path, buf, size, &size) != 0) {
        kfree(buf); fds[fd].used = 0; return (u64)-1;
    }
    fds[fd].data = buf;
    fds[fd].size = size;
    fds[fd].pos = 0;
    return (u64)fd;
}

static u64 do_seek(int fd, i64 off, int whence)
{
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used) return (u64)-1;
    i64 base = whence == 1 ? fds[fd].pos : (whence == 2 ? fds[fd].size : 0);
    i64 np = base + off;
    if (np < 0) np = 0;
    if (np > (i64)fds[fd].size) np = fds[fd].size;
    fds[fd].pos = (u32)np;
    return (u64)np;
}

/* Linux x86-64 syscall numbers, translated to the internal ones. glibc and
   any normal Linux binary use these, not our numbering. */
static u64 linux_to_internal(u64 nr)
{
    switch (nr) {
    case 0:  return SYS_READ;
    case 1:  return SYS_WRITE;
    case 2:  return SYS_OPEN;
    case 3:  return SYS_CLOSE;
    case 5:  return SYS_SEEK;     /* fstat: stubbed via seek semantics */
    case 8:  return SYS_SEEK;     /* lseek */
    case 9:  return SYS_MMAP;     /* mmap  */
    case 10: return SYS_MPROTECT;
    case 11: return SYS_MUNMAP;
    case 12: return SYS_BRK;
    case 39: return SYS_GETPID;
    case 60: return SYS_EXIT;
    case 79: return SYS_GETCWD;   /* getcwd */
    case 80: return SYS_CHDIR;    /* chdir */
    case 87: return SYS_UNLINK;
    case 102: return SYS_GETPID;  /* getuid */
    case 110: return SYS_GETPID;  /* getppid */
    case 13: return SYS_MPROTECT;   /* rt_sigaction: ignore */
    case 14: return SYS_MPROTECT;   /* rt_sigprocmask: ignore */
    case 16: return SYS_IOCTL;
    case 20: return SYS_MPROTECT;   /* writev: ignore */
    case 21: return SYS_ACCESS;     /* access */
    case 89: return SYS_READLINK;
    case 158: return SYS_ARCH_PRCTL;
    case 186: return SYS_GETPID;    /* gettid */
    case 201: return SYS_TIME;      /* time */
    case 202: return SYS_FUTEX;
    case 218: return SYS_SET_TID;   /* set_tid_address */
    case 228: return SYS_TICKS;     /* clock_gettime */
    case 231: return SYS_EXIT;      /* exit_group */
    case 257: return SYS_OPENAT;    /* openat */
    case 267: return SYS_READLINK;  /* readlinkat */
    case 273: return SYS_YIELD;     /* set_robust_list: nothing to track */
    case 302: return SYS_YIELD;     /* prlimit64: no limits enforced */
    case 318: return SYS_GETRANDOM;
    case 334: return SYS_GETRANDOM; /* rseq */
    default:  return nr;
    }
}

u64 syscall_dispatch(struct syscall_frame *f)
{
    if (cur_abi == NEO_ABI_LINUX) f->nr = linux_to_internal(f->nr);
    switch (f->nr) {
    case SYS_EXIT:
        process_exit((int)f->a0);
        return 0;                     /* not reached */
    case SYS_WRITE:
        return do_write((int)f->a0, (const void *)f->a1, (u32)f->a2);
    case SYS_READ:
        return do_read((int)f->a0, (void *)f->a1, (u32)f->a2);
    case SYS_OPEN:
    case SYS_OPENAT:
        return do_open((const char *)f->a0, f->a1);
    case SYS_CLOSE:
        if (f->a0 < FD_MAX && f->a0 >= 3) fds[f->a0].used = 0;
        return 0;
    case SYS_SEEK:
        return do_seek((int)f->a0, (i64)f->a1, (int)f->a2);
    case SYS_BRK:
        return process_sbrk((isize)f->a0);
    case SYS_MMAP:
        return process_mmap(f->a0, f->a1, f->a2, (int)f->a3);
    case SYS_MPROTECT:
        return process_mprotect(f->a0, f->a1, f->a2);
    case SYS_ARCH_PRCTL:
        return process_arch_prctl((int)f->a0, f->a1);
    case SYS_SET_TID:
        return (u64)process_current_pid();
    case SYS_FUTEX:
        /* Single-threaded: a futex wait can never be satisfied, so report
           EAGAIN rather than blocking forever. */
        return (u64)-11;
    case SYS_GETRANDOM:
        return process_getrandom((void *)f->a0, (u32)f->a1);
    case SYS_ACCESS:
        return 0;
    case SYS_READLINK:
    case SYS_STAT:
        return (u64)-1;
    case SYS_IOCTL:
        return (u64)-25;   /* ENOTTY */
    case SYS_MUNMAP:
        return 0;
    case SYS_GETPID:
        return (u64)process_current_pid();
    case SYS_TIME: {
        struct hw_profile *hw = device_profile();
        if (!hw || !hw->has_rtc) return 0;
        return (u64)hw->rtc_hour * 3600 + (u64)hw->rtc_minute * 60 + hw->rtc_second;
    }
    case SYS_TICKS:
        return irq_get_ticks();
    case SYS_CHDIR:
        if (f->a0) { strlcpy(cur_cwd, (const char *)f->a0, sizeof(cur_cwd)); return 0; }
        return (u64)-1;
    case SYS_GETCWD: {
        char *buf = (char *)f->a0;
        if (!buf) return (u64)-1;
        strlcpy(buf, cur_cwd, (usize)f->a1);
        return 0;
    }
    case SYS_UNAME: {
        char *buf = (char *)f->a0;
        if (!buf) return (u64)-1;
        strlcpy(buf, cur_abi == NEO_ABI_LINUX ? "Linux" : "NiteKernel",
                (usize)f->a1);
        return 0;
    }
    case SYS_YIELD:
        return 0;
    case SYS_ABI:
        return (u64)cur_abi;
    case SYS_UNLINK:
        if (!f->a0) return (u64)-1;
        return neofs_delete_file((const char *)f->a0) == 0 ? 0 : (u64)-1;
    case SYS_GETARGS:
        return process_write_args((char *)f->a0, (u32)f->a1);
    case SYS_GETENV:
        return (u64)-1;
    default:
        klog_warn("unknown syscall %u from pid %d", (u32)f->nr, process_current_pid());
        return (u64)-1;
    }
}
