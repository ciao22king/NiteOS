#ifndef NEO_SYSCALL_H
#define NEO_SYSCALL_H

#include <nitekernel.h>

/* int 0x80 ABI. Arguments in RDI, RSI, RDX, RCX, R8, R9; result in RAX.
   This mirrors the Linux x86-64 register order (R10 is arg3 there, but RCX is
   the 4th here so the same numbering can be reused for the PE/native paths). */
#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_BRK         5
#define SYS_GETPID      6
#define SYS_TIME        7
#define SYS_CHDIR       8
#define SYS_GETCWD      9
#define SYS_UNAME      10
#define SYS_YIELD      11
#define SYS_MMAP       12
#define SYS_MUNMAP     13
#define SYS_GETARGS    14
#define SYS_GETENV     15
#define SYS_OPENAT     16
#define SYS_SEEK       17
#define SYS_UNLINK     18
#define SYS_MKDIR      19
#define SYS_GETDENTS   20
#define SYS_TICKS      21
#define SYS_ABI        22
#define SYS_ARCH_PRCTL 23
#define SYS_SET_TID    24
#define SYS_FUTEX      25
#define SYS_MPROTECT   26
#define SYS_GETRANDOM  27
#define SYS_READLINK   28
#define SYS_ACCESS     29
#define SYS_STAT       30
#define SYS_IOCTL      31
#define SYS_FSTAT      32
#define SYS_FACCESSAT  33
#define SYS_WRITEV     34
#define SYS_GETUID     35
#define SYS_GETTIMEOFDAY 36
#define SYS_ENOSYS     37
#define SYS_NOOP       38
#define SYS_FSTAT_DESC 39
#define SYS_SELECT     40
#define SYS_POLL       41

/* ABI identifiers reported by SYS_ABI, used by the loaders to pick a personality. */
#define NEO_ABI_NATIVE 0
#define NEO_ABI_LINUX  1
#define NEO_ABI_WIN32  2

/* Frame pushed by syscall_stub (keep in sync with src/arch/usermode.asm). */
struct syscall_frame {
    u64 nr;
    u64 a0, a1, a2, a3;
    u64 r8, r9, r10, r11;
    u64 r15, r14, r13, r12, rbp, rbx;
};

void syscall_init(void);
u64  syscall_dispatch(struct syscall_frame *f);

/* Process file-descriptor table. 0/1/2 are the console unless the shell
   redirects them into buffers. */
void syscall_reset_fds(void);
void syscall_set_stdio(const u8 *in, u32 in_len, u8 *out, u32 out_cap, u32 *out_len);
u32  syscall_out_length(void);
const char *syscall_cwd(void);

/* Set by the loader before entering ring 3. */
void syscall_set_abi(int abi);
int  syscall_get_abi(void);
void syscall_set_args(int argc, char **argv);
void syscall_set_cwd(const char *dir);
void syscall_set_kernel_stack(u64 top);

#endif
