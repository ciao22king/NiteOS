BITS 64
section .text
global gdt_flush

; void gdt_flush(struct gdt_ptr *ptr)
gdt_flush:
    lgdt [rdi]
    mov ax, 0x10          ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Reload CS via a far return to a 64-bit code segment.
    pop rax
    push 0x08
    push rax
    retfq
