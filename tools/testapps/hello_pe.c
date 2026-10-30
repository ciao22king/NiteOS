/* Freestanding PE32+ (Windows x86-64) test program.
   It has no imports, so NiteKernel's PE loader can run it directly.
   Build: x86_64-w64-mingw32-gcc -nostdlib -ffreestanding -e neo_entry \
              -Wl,--subsystem,console -o hello_pe.exe hello_pe.c */

typedef unsigned long long u64;

static long sys(u64 nr, u64 a0, u64 a1, u64 a2)
{
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory");
    return ret;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }

/* Windows x64 entry point signature: (PIMAGE_TLSCALLBACK, DWORD, PVOID). */
void neo_entry(void *hinst, unsigned long reason, void *reserved)
{
    (void)hinst; (void)reason; (void)reserved;
    const char *msg = "hello from a Windows PE32+ .exe on NiteKernel\n";
    sys(1, 1, (u64)msg, slen(msg));
    const char *m2 = "PE: exiting with code 7\n";
    sys(1, 1, (u64)m2, slen(m2));
    sys(0, 7, 0, 0);
    for (;;) ;
}
