#ifndef NEO_PROCESS_H
#define NEO_PROCESS_H

#include <nitekernel.h>
#include <mm.h>

/* Process personalities. The same loader/exit path serves all three; only the
   syscall ABI and the entry conventions differ. */
#define PROC_KIND_NATIVE 0   /* .lash */
#define PROC_KIND_LINUX  1   /* ELF64  */
#define PROC_KIND_WIN32  2   /* PE32+  */

#define PROC_MAX_ARGS 16
#define PROC_MAX_ENV  16

struct process {
    int   pid;
    int   kind;
    char  name[64];
    struct addr_space *space;
    u64   entry;
    u64   user_stack_top;
    /* Load bias and program headers, published to the auxv. */
    u64   base;
    u64   phdr;
    u32   phnum;

    /* Program break for brk/mmap. */
    u64   brk_base;
    u64   brk_cur;
    u64   brk_limit;

    int   argc;
    char  argv[PROC_MAX_ARGS][128];
    int   envc;
    char  env[PROC_MAX_ENV][128];
    char  cwd[128];

    int   exit_code;
    int   running;
    /* Where the loader left the final kernel-stack frame, so exit can return. */
    u64   kstack_return;
};

void  process_init(void);
struct process *process_current(void);
int   process_current_pid(void);

/* Load and run a program from a NEOFS file. `kind` is PROC_KIND_*. Returns the
   exit code, or a negative value if the image could not be loaded. */
int   process_run_file(const char *path, int kind, int argc, char **argv);
int   process_exec_image(const char *name, const u8 *image, u32 size,
                         int argc, char **argv);

/* Called from the syscall layer. */
void  process_exit(int code) __attribute__((noreturn));
isize process_sbrk(isize increment);
int   process_handle_fault(u64 cr2);
u64   process_mmap(u64 addr, u64 len, u64 prot, int flags);
/* Reserve the virtual range an ET_DYN image needs; returns the chosen base. */
u64   process_reserve_image(struct process *p, u64 vbase, u64 span);
int   process_mprotect(u64 addr, u64 len, u64 prot);
int   process_arch_prctl(int code, u64 addr);
u64   process_getrandom(void *buf, u32 len);
u64   process_write_args(char *buf, u32 len);

/* Entry trampoline: the loaders set up the frame then jump here. */
void  process_enter_user(void);

#endif
