/* Freestanding ELF64 test program: talks to NiteKernel over int 0x80.
   Build: gcc -nostdlib -static -no-pie -ffreestanding -Wl,-e,_start \
              -Wl,--build-id=none -o hello_elf hello_elf.c */
typedef unsigned long u64;

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

void _start(void)
{
    const char *msg = "hello from ELF64 on NiteKernel\n";
    /* Linux numbers: an ELF image is entered with the Linux ABI, so exit is 60,
       not 0. Using 0 here means read, and the process spins forever. */
    sys(1, 1, (u64)msg, slen(msg));
    const char *m2 = "ELF: exiting with code 42\n";
    sys(1, 1, (u64)m2, slen(m2));
    sys(60, 42, 0, 0);
    for (;;) ;
}
