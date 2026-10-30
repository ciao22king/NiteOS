; Trampoline template for a Windows API stub.
;
; A PE image calls its imports through the IAT. The entries we patch point at a
; copy of this code placed in a user-mapped page, so the call stays at CPL 3 and
; traps into the kernel with `int 0x80`; the kernel then runs the emulation at
; CPL 0. The Windows register convention (rcx, rdx, r8, r9) is reshuffled into
; the frame the int 0x80 stub captures (rdi, rsi, rdx, rcx).
;
; rdi and rsi are callee-saved under the Windows ABI, so they are preserved.
; The mov eax, imm32 immediate is patched with the per-function syscall id.
BITS 64
section .text
global win32_stub_template
global win32_stub_template_end
win32_stub_template:
    push rdi
    push rsi
    mov r11, rdx
    mov rdi, rcx
    mov rsi, r11
    mov r11, r8
    mov rdx, r11
    mov rcx, r9
    mov eax, 0x11223344
    int 0x80
    pop rsi
    pop rdi
    ret
win32_stub_template_end:
