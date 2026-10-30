#ifndef NEO_WIN32_H
#define NEO_WIN32_H

#include <nitekernel.h>

struct process;

/* Windows personality: a PE image calls its imports through the IAT, so each
   import is bound to a stub that traps into the kernel and runs the emulation
   here. */

void win32_init(void);

/* Called by the PE loader for every imported symbol. Returns the address to
   write into the IAT, or 0 when the symbol is not emulated. */
u64  win32_bind_import(const char *name);

/* Install the thread environment block for the current process. */
void win32_set_tls(void);

/* Dispatch one emulated API call (the stub's trap). */
u64  win32_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3);

#endif
