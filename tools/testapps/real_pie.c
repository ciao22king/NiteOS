/* A real static-PIE (ET_DYN) executable.

   Proves the loader handles a position-independent image: it must reserve the
   image at a base of its choosing and apply the R_X86_64_RELATIVE relocations
   before handing control to a libc that assumes the relocations are done.

   Build: gcc -static-pie -O2 -o pie real_pie.c
*/
#include <stdio.h>

int main(void)
{
    volatile int value = 42;
    printf("static PIE ok: %d\n", value);
    return 0;
}
