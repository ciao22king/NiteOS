#include <syscall.h>
#include <win32.h>
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

int syscall_trace = 0;
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

static int syscall_stdout_sink(char c);

void syscall_init(void)
{
    idt_set_gate(0x80, (u64)syscall_stub, 3);   /* DPL 3: callable from ring 3 */
    syscall_msr_init();
    /* Shell redirection: console_putc() offers every character to this hook. */
    console_set_sink(syscall_stdout_sink);
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
    int   owns;          /* `data` was allocated by open() and must be freed */
    u8   *data;
    u32   size;
    u32   pos;
};

static struct fd_entry fds[FD_MAX];

/* stdout is a buffer: keep the character instead of rendering it. Returning 1
   (even when the buffer is full) stops it reaching the screen. */
static int syscall_stdout_sink(char c)
{
    if (!fds[1].data) return 0;
    if (fds[1].pos < fds[1].size) fds[1].data[fds[1].pos++] = (u8)c;
    return 1;
}

static void fd_release(int fd)
{
    if (fd < 0 || fd >= FD_MAX) return;
    if (fds[fd].owns && fds[fd].data) kfree(fds[fd].data);
    memset(&fds[fd], 0, sizeof(fds[fd]));
}

static int fd_alloc(void)
{
    for (int i = 3; i < FD_MAX; i++)
        if (!fds[i].used) { fd_release(i); fds[i].used = 1; return i; }
    return -1;
}

void syscall_reset_fds(void)
{
    /* Release the buffers open() allocated and drop every redirection. The
       reset used to memset the table only, so each program that opened a file
       leaked its copy of the image into the kernel heap. */
    for (int i = 0; i < FD_MAX; i++) fd_release(i);
    /* 0,1,2 are the console and are always live. */
    fds[0].used = fds[1].used = fds[2].used = 1;
}

void syscall_set_stdio(const u8 *in, u32 in_len, u8 *out, u32 out_cap, u32 *out_len)
{
    /* Only the streams named by the caller are reprogrammed. Resetting the
       whole table here turned the other half of a nested redirection off, so
       `cat < file > out` lost its capture buffer. */
    if (in) {
        fds[0].data = (u8 *)in;
        fds[0].size = in_len;
        fds[0].pos = 0;
        fds[0].owns = 0;
        fds[0].used = 1;
    }
    if (out) {
        fds[1].data = out;
        fds[1].size = out_cap;
        fds[1].pos = 0;
        fds[1].owns = 0;
        fds[1].used = 1;
        fds[2].data = out;
        fds[2].size = out_cap;
        fds[2].pos = 0;
        fds[2].used = 1;
    }
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
        /* Interactive console. Block for the first byte, then return
           whatever else is already queued (stopping at a newline). The
           blocking-first-byte rule matters: readline issues read(0, &c, 1)
           and a zero-length result would look like EOF and exit the shell. */
        char *out = (char *)buf;
        if (len == 0) return 0;
        int c;
        while ((c = console_getchar()) < 0) hlt();
        u32 n = 0;
        out[n++] = (char)c;
        if (c == '\n') return n;
        while (n < len) {
            c = console_getchar();
            if (c < 0) break;
            out[n++] = (char)c;
            if (c == '\n') break;
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

int syscall_stdin_redirected(void) { return fds[0].data != NULL; }

long syscall_stdin_read(void *buf, u32 len)
{
    return (long)do_read(0, buf, len);
}

/* access(path, mode): report existence only. mode is ignored. */
static u64 do_access(const char *path)
{
    if (!path) return (u64)-1;
    return neofs_file_exists(path) ? 0 : (u64)-2;   /* ENOENT */
}

struct iovec_s { u64 base; u64 len; };

static u64 do_writev(int fd, const struct iovec_s *iov, int cnt)
{
    u64 total = 0;
    for (int i = 0; i < cnt; i++) {
        long r = (long)do_write(fd, (const void *)iov[i].base, (u32)iov[i].len);
        if (r < 0) return (u64)r;
        total += (u64)r;
    }
    return total;
}

/* Minimal Linux struct stat: callers here only look at st_mode and st_size. */
static u64 do_stat(const char *path, void *buf)
{
    if (!path || !buf) return (u64)-14;             /* EFAULT */
    long sz = neofs_file_size(path);
    if (sz < 0) return (u64)-2;                     /* ENOENT */
    u8 *st = (u8 *)buf;
    memset(st, 0, 144);
    *(u32 *)(st + 0)  = 0;                          /* st_dev */
    *(u64 *)(st + 8)  = 1;                          /* st_ino */
    *(u32 *)(st + 16) = 0x81A4;                     /* st_mode: S_IFREG|0644 */
    *(u64 *)(st + 24) = 1;                          /* st_nlink */
    *(u32 *)(st + 28) = 0;                          /* st_uid */
    *(u32 *)(st + 32) = 0;                          /* st_gid */
    *(u64 *)(st + 48) = (u64)sz;                    /* st_size */
    *(u64 *)(st + 56) = 4096;                       /* st_blksize */
    *(u64 *)(st + 64) = ((u64)sz + 511) / 512;      /* st_blocks */
    return 0;
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
    fds[fd].owns = 1;
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
    case 8:  return SYS_SEEK;     /* lseek */
    case 9:  return SYS_MMAP;     /* mmap  */
    case 10: return SYS_MPROTECT;
    case 11: return SYS_MUNMAP;
    case 12: return SYS_BRK;
    case 39: return SYS_GETPID;
    case 96: return SYS_GETTIMEOFDAY;
    case 102: return SYS_GETUID;   /* getuid  */
    case 104: return SYS_GETUID;   /* getgid  */
    case 107: return SYS_GETUID;   /* geteuid */
    case 108: return SYS_GETUID;   /* getegid */
    case 111: return SYS_GETUID;   /* getpgrp */
    case 118: return SYS_YIELD;    /* setresuid: nothing to drop */
    case 119: return SYS_YIELD;    /* setresgid */
    case 117: return SYS_ENOSYS;   /* ptrace */
    case 121: return SYS_YIELD;    /* setpgid */
    case 112: return SYS_YIELD;    /* setsid  */
    case 33:  return SYS_ENOSYS;   /* dup2: no fd duplication */
    case 32:  return SYS_ENOSYS;   /* dup  */
    case 60: return SYS_EXIT;
    case 79: return SYS_GETCWD;   /* getcwd */
    case 80: return SYS_CHDIR;    /* chdir */
    case 87: return SYS_UNLINK;
    case 110: return SYS_GETUID;  /* getppid */
    case 63: return SYS_UNAME;      /* uname: struct utsname under Linux */
    case 13: return SYS_NOOP;       /* rt_sigaction: no signal delivery */
    case 14: return SYS_NOOP;       /* rt_sigprocmask */
    case 16: return SYS_IOCTL;
    case 20: return SYS_WRITEV;
    case 24: return SYS_YIELD;      /* sched_yield */
    case 72: return SYS_NOOP;       /* fcntl: no descriptor flags to keep */
    case 109: return SYS_NOOP;      /* setpgid */
    case 41: return SYS_ENOSYS;     /* socket: no network stack for user progs */
    case 7:  return SYS_POLL;       /* poll  */
    case 23: return SYS_SELECT;     /* select */
    case 270: return SYS_SELECT;    /* pselect6 */
    case 4:  return SYS_FSTAT;      /* stat  */
    case 5:  return SYS_FSTAT_DESC; /* fstat */
    case 6:  return SYS_FSTAT;      /* lstat */
    case 21: return SYS_ACCESS;     /* access  */
    case 262: return SYS_STAT;      /* newfstatat */
    case 269: return SYS_FACCESSAT; /* faccessat   */
    case 439: return SYS_FACCESSAT; /* faccessat2  */
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
    extern int syscall_trace;
    if (cur_abi == NEO_ABI_WIN32)
        return win32_syscall(f->nr, f->a0, f->a1, f->a2, f->a3);
    u64 raw_nr = f->nr;
    if (cur_abi == NEO_ABI_LINUX) f->nr = linux_to_internal(f->nr);
    if (syscall_trace)
        klog_info("TRACE", "raw=%u -> int=%u a0=%p a1=%p a2=%p",
                  (u32)raw_nr, (u32)f->nr, (void *)f->a0, (void *)f->a1, (void *)f->a2);
    switch (f->nr) {
    case SYS_EXIT:
        process_exit((int)f->a0);
        return 0;                     /* not reached */
    case SYS_WRITE:
        return do_write((int)f->a0, (const void *)f->a1, (u32)f->a2);
    case SYS_READ:
        return do_read((int)f->a0, (void *)f->a1, (u32)f->a2);
    case SYS_OPEN:
        return do_open((const char *)f->a0, f->a1);
    case SYS_OPENAT:
        /* openat(dirfd, path, flags, mode): the path is the *second* argument.
           AT_FDCWD (-100) means "relative to cwd", which is what we do anyway. */
        return do_open((const char *)f->a1, f->a2);
    case SYS_CLOSE:
        if (f->a0 < FD_MAX && f->a0 >= 3) fd_release((int)f->a0);
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
    case SYS_GETUID:
    case SYS_NOOP:
        return 0;
    case SYS_SELECT:
        /* Readline calls select() before read() to decide whether stdin has
           data. Reporting "not readable" would look like EOF and exit the
           shell, so stdin is always advertised as ready; the subsequent
           read() blocks properly. */
        if (f->a1) *(u64 *)f->a1 = 1;       /* readfds: fd 0 */
        return 1;
    case SYS_POLL: {
        /* poll(struct pollfd *fds, nfds_t nfds, int timeout) */
        struct pollfd_s { int fd; short events; short revents; } *pf;
        pf = (struct pollfd_s *)f->a0;
        int n = (int)f->a1, ready = 0;
        for (int i = 0; i < n; i++) {
            pf[i].revents = (pf[i].fd == 0) ? 1 : 0;   /* POLLIN on stdin */
            if (pf[i].revents) ready++;
        }
        return ready;
    }
    case SYS_ENOSYS:
        return (u64)-1;
    case SYS_GETTIMEOFDAY: {
        /* gettimeofday(struct timeval *tv, void *tz) */
        if (f->a0) {
            u8 *tv = (u8 *)f->a0;
            memset(tv, 0, 16);
            struct hw_profile *hw = device_profile();
            if (hw && hw->has_rtc)
                *(u64 *)tv = (u64)hw->rtc_hour * 3600 +
                             (u64)hw->rtc_minute * 60 + hw->rtc_second;
        }
        return 0;
    }
    case SYS_ACCESS:
        /* access(path, mode): the path is the first argument. */
        return do_access((const char *)f->a0);
    case SYS_FACCESSAT:
        /* faccessat(dirfd, path, mode, flags): path is the second. */
        return do_access((const char *)f->a1);
    case SYS_STAT:
        /* newfstatat(dirfd, path, buf, flags) */
        return do_stat((const char *)f->a1, (void *)f->a2);
    case SYS_FSTAT:
        /* stat(path, buf) */
        return do_stat((const char *)f->a0, (void *)f->a1);
    case SYS_FSTAT_DESC: {
        /* fstat(fd, buf): only the size is meaningful for our fds. */
        int fd = (int)f->a0;
        if (fd < 0 || fd >= FD_MAX || !fds[fd].used) return (u64)-9;  /* EBADF */
        u8 *st = (u8 *)f->a1;
        if (!st) return (u64)-14;
        memset(st, 0, 144);
        *(u64 *)(st + 8)  = (u64)fd;
        *(u32 *)(st + 16) = 0x21A4;                 /* S_IFCHR|0644 */
        *(u64 *)(st + 24) = 1;
        *(u64 *)(st + 48) = fds[fd].size;
        *(u64 *)(st + 56) = 4096;
        return 0;
    }
    case SYS_WRITEV:
        return do_writev((int)f->a0, (const struct iovec_s *)f->a1, (int)f->a2);
    case SYS_READLINK:
        return (u64)-1;
    case SYS_IOCTL:
        /* A minimal tty: TCGETS and TIOCGWINSZ are answered so isatty() is
           true and bash behaves as an interactive shell. Anything else is
           ENOTTY, as on a real console without that capability. */
        switch (f->a1) {
        case 0x5401: {                          /* TCGETS */
            u8 *t = (u8 *)f->a2;
            if (t) {
                /* The kernel-side struct termios (asm/termbits.h, NCCS=19) is
                   36 bytes; glibc's copy is 60. Writing 60 from here clobbers
                   the 24 bytes past it, which is usually the caller's stack
                   canary. Write exactly what the kernel would. */
                memset(t, 0, 36);
                *(u32 *)(t + 0) = 0x0500;        /* c_iflag: ICRNL */
                *(u32 *)(t + 4) = 0x0005;        /* c_oflag: OPOST|ONLCR */
                *(u32 *)(t + 8) = 0x00BF;        /* c_cflag: CS8|CREAD */
                *(u32 *)(t + 12) = 0x8A3B;       /* c_lflag: ISIG|ICANON|ECHO */
            }
            return 0;
        }
        case 0x5413: {                          /* TIOCGWINSZ */
            u16 *ws = (u16 *)f->a2;
            if (ws) { ws[0] = 80; ws[1] = 25; ws[2] = 0; ws[3] = 0; }
            return 0;
        }
        case 0x540B:                            /* TCSETS: accept, ignore */
        case 0x540E:                            /* TIOCSCTTY */
            return 0;
        default:
            return (u64)-25;                    /* ENOTTY */
        }
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
        /* getcwd(buf, size) returns the number of bytes written *including*
           the NUL. Returning 0 makes callers treat it as failure. */
        char *buf = (char *)f->a0;
        if (!buf) return (u64)-1;
        u32 n = (u32)strlen(cur_cwd) + 1;
        if (f->a1 && n > (u32)f->a1) return (u64)-34;   /* ERANGE */
        memcpy(buf, cur_cwd, n);
        return n;
    }
    case SYS_UNAME: {
        char *buf = (char *)f->a0;
        if (!buf) return (u64)-1;
        if (cur_abi == NEO_ABI_LINUX) {
            /* struct utsname: six 65-byte fields. Programs parse these. */
            memset(buf, 0, 6 * 65);
            strlcpy(buf + 0 * 65, "Linux", 65);
            strlcpy(buf + 1 * 65, "nitekernel", 65);
            strlcpy(buf + 2 * 65, "6.6.0-nitekernel", 65);
            strlcpy(buf + 3 * 65, "#1 SMP NiteKernel", 65);
            strlcpy(buf + 4 * 65, "x86_64", 65);
            strlcpy(buf + 5 * 65, "localdomain", 65);
        } else {
            strlcpy(buf, "NiteKernel", (usize)f->a1);
        }
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
