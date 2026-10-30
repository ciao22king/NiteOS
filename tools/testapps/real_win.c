/* A real Windows console .exe built with the mingw-w64 toolchain.

   Unlike the freestanding PE test this one is produced by a normal compiler
   driver, so the kernel sees the section layout, subsystem and entry point
   that Windows tooling actually emits.

   Build: x86_64-w64-mingw32-gcc -O2 -static -o real_win.exe real_win.c
*/
#include <stdio.h>

int main(void)
{
    printf("real mingw PE .exe on NiteKernel\n");
    return 7;
}
