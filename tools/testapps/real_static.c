/* A real, unmodified glibc static executable.

   This is the "can it run a program I did not write for it" test: the kernel
   has to satisfy everything a normal dynamically-initialised glibc program
   asks for (auxv, TLS, brk, mmap, mprotect, AT_RANDOM, ...). Build with:

     gcc -static -O2 -o real_static real_static.c
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    char *p = malloc(64);
    if (!p) {
        puts("malloc failed");
        return 1;
    }
    strcpy(p, "REAL STATIC OK");
    printf("%s\n", p);
    free(p);

    /* Exercise a little more of the runtime so the exit path is real too. */
    for (int i = 0; i < 3; i++)
        printf("iteration %d\n", i);

    return 0;
}
