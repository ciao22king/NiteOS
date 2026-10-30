; Multiboot2 header + 32-bit entry stub.
; GRUB drops us in protected mode with paging off; we build identity + higher
; half page tables, enter long mode, then jump to kmain (never returns).

BITS 32

MB2_MAGIC    equ 0xE85250D6
MB2_ARCH     equ 0                    ; i386 protected mode
MB2_TAG_END  equ 0
MB2_TAG_FB   equ 8

section .multiboot2
align 8
mb2_start:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd mb2_end - mb2_start
    dd -(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))

    ; --- framebuffer request: ask GRUB to set a linear framebuffer ---
align 8
    dw MB2_TAG_FB
    dw 1                          ; optional: boot even if no video device
    dd 20
    dd 0                          ; width  = 0 -> any
    dd 0                          ; height = 0 -> any
    dd 32                         ; depth  = 32 bpp

    ; --- end tag ---
align 8
    dw MB2_TAG_END
    dw 0
    dd 8
mb2_end:

; ---------------------------------------------------------------------------
section .boot
global _start
extern kmain

_start:
    cli
    mov esp, stack_top_early
    mov [mb2_magic_saved], eax    ; multiboot2 magic
    mov esi, ebx                  ; multiboot2 info pointer (physical, < 4 GiB)
    call setup_page_tables
    jmp enter_long_mode           ; does not return

; ---------------------------------------------------------------------------
setup_page_tables:
    ; Zero 4 pages: PML4, PDPT, PD, spare.
    mov edi, pml4
    mov cr3, edi
    xor eax, eax
    mov ecx, 4096 * 4 / 4
    rep stosd

    ; PML4[0]   -> pdpt : identity map of the low 1 GiB
    mov eax, pdpt
    or  eax, 0x03
    mov [pml4], eax
    ; PML4[511] -> pdpt : 0xFFFFFFFF80000000 onward (kernel higher half)
    mov [pml4 + 511*8], eax

    ; PDPT index 0   -> pd : low 1 GiB identity
    ; PDPT index 510 -> pd : covers 0xFFFFFFFF80000000 (higher-half base)
    mov eax, pd
    or  eax, 0x03
    mov [pdpt], eax
    mov [pdpt + 510*8], eax

    ; Fill PD with 512 * 2 MiB pages (present|writable|huge).
    xor ecx, ecx
    mov eax, 0x83
.fill_pd:
    mov [pd + ecx*8], eax
    add eax, 0x200000
    inc ecx
    cmp ecx, 512
    jb .fill_pd
    ret

; ---------------------------------------------------------------------------
enter_long_mode:
    mov eax, cr4
    or  eax, (1 << 5)             ; PAE
    ; OSFXSR | OSXMMEXCPT: without these every SSE instruction raises #UD,
    ; and user-space libc uses SSE2 throughout.
    or  eax, (1 << 9) | (1 << 10)
    mov cr4, eax

    ; SSE state: clear CR0.EM (bit 2) and CR0.TS (bit 3), set CR0.MP (bit 1).
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or  eax, (1 << 1)
    mov cr0, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080           ; EFER
    rdmsr
    or  eax, (1 << 8) | (1 << 11) ; LME | NXE (bit 63 in PTEs is NX, not reserved)
    wrmsr

    mov eax, cr0
    or  eax, (1 << 31) | (1 << 0) ; PG | PE
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:.long_entry

BITS 64
.long_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top_early_hi
    mov edi, [mb2_magic_saved]
    mov esi, ebx                  ; zero-extends to rsi
    call kmain

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
section .boot.data
align 4096
pml4:       resb 4096
pdpt:       resb 4096
pd:         resb 4096
spare:      resb 4096
align 16
stack_bottom_early: resb 16384
stack_top_early:
stack_top_early_hi equ stack_top_early
align 8
mb2_magic_saved:    dq 0

align 16
gdt64:
    dq 0                          ; null
    dq 0x00AF9A000000FFFF         ; 0x08 kernel code
    dq 0x00CF92000000FFFF         ; 0x10 kernel data
gdt64_ptr:
    dw gdt64_ptr - gdt64 - 1
    dq gdt64
