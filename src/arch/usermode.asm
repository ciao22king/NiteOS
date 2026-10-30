BITS 64
section .text

; void usermode_enter(u64 entry, u64 user_rsp)
;   rdi = entry point (user RIP), rsi = user stack top
; Builds an iretq frame that drops to ring 3 with interrupts enabled.
global usermode_enter
usermode_enter:
    cli
    mov ax, 0x1b            ; user data selector (index 3 << 3 | RPL 3)
    mov ds, ax
    mov es, ax
    ; FS and GS are deliberately not loaded: in long mode `mov gs, ax` sets
    ; the segment base to 0, which would discard the value the Windows
    ; personality put in the MSR (the TEB pointer).

    push qword 0x1b         ; SS (RPL 3 is mandatory for a ring-3 iretq)
    push rsi                ; RSP
    push qword 0x202        ; RFLAGS: IF=1, bit1 always set
    push qword 0x23         ; CS  (index 4 << 3 | RPL 3)
    push rdi                ; RIP

    ; A Linux process starts with a defined register state: the C runtime reads
    ; rdx as the atexit function pointer and r9 as rtld_fini. Inheriting stale
    ; kernel values makes glibc register a garbage exit handler.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; The syscall interrupt gate (int 0x80) lands here. On entry the CPU has
; switched to the kernel stack from TSS.rsp0 and pushed SS,RSP,RFLAGS,CS,RIP.
global syscall_stub
extern syscall_dispatch
syscall_stub:
    swapgs                  ; harmless without FSGSBASE use, keeps symmetry
    push qword 0            ; pad so the frame is 16-byte aligned for the call
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    ; Push in reverse so the frame layout matches struct syscall_frame:
    ;   [rsp+0]=nr  [rsp+8]=a0(rdi)  [rsp+16]=a1(rsi)
    ;   [rsp+24]=a2(rdx)  [rsp+32]=a3(rcx)  then r8,r9,r10,r11
    push r11                ; caller RFLAGS
    push r10
    push r9
    push r8
    push rcx                ; arg3
    push rdx                ; arg2
    push rsi                ; arg1
    push rdi                ; arg0
    push rax                ; syscall number
    cld

    mov rdi, rsp
    call syscall_dispatch

    ; syscall_dispatch returns the value to hand back in RAX.
    add rsp, 8              ; drop the saved syscall number
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop r8
    pop r9
    pop r10
    pop r11
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    add rsp, 8              ; drop the alignment pad
    swapgs
    iretq

; ---- syscall instruction entry (Linux ABI) ----
; On entry from the `syscall` instruction: RCX = return RIP, R11 = RFLAGS,
; RSP is still the user stack. Arguments follow the Linux convention
; (rdi, rsi, rdx, r10, r8, r9), so arg3 comes from r10, not rcx.
global syscall_entry
extern syscall_dispatch
section .data
syscall_user_rsp:   dq 0
syscall_user_rip:   dq 0
syscall_user_rflags: dq 0
section .text
syscall_entry:
    swapgs
    mov [rel syscall_user_rsp], rsp
    mov [rel syscall_user_rip], rcx
    mov [rel syscall_user_rflags], r11
    mov rsp, [rel syscall_kstack_top]     ; switch to the kernel stack
    sti
    push qword 0
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push r11
    push r10
    push r9
    push r8
    push r10                ; arg3 (Linux: r10)
    push rdx                ; arg2
    push rsi                ; arg1
    push rdi                ; arg0
    push rax                ; syscall number
    cld
    mov rdi, rsp
    call syscall_dispatch
    add rsp, 8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop r8
    pop r9
    pop r10
    pop r11
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    add rsp, 8
    mov rsp, [rel syscall_user_rsp]
    mov rcx, [rel syscall_user_rip]
    mov r11, [rel syscall_user_rflags]
    swapgs
    o64 sysret

section .data
global syscall_kstack_top
syscall_kstack_top: dq 0

section .note.GNU-stack noalloc noexec nowrite progbits

; Kernel context saved before dropping to ring 3 so a user exit (int 0x80
; SYS_EXIT) can unwind back into process_enter_user's caller.
global user_ctx_save
global user_ctx_restore
global user_ctx
section .data
user_ctx: times 8 dq 0
section .text

; void user_ctx_save(u64 entry, u64 user_rsp, u64 cr3, u64 kstack_top)
;   Saves the kernel context, moves onto the per-process kernel stack (which
;   lives in the kernel half and is therefore present in the new address
;   space), then loads the new CR3 and drops to ring 3. Switching the stack
;   first matters: the caller's stack is in low memory, mapped only by the
;   kernel PML4's identity entry, so a `ret` after the CR3 load would fault.
user_ctx_save:
    mov [user_ctx + 0], rsp
    mov [user_ctx + 8], rbx
    mov [user_ctx + 16], rbp
    mov [user_ctx + 24], r12
    mov [user_ctx + 32], r13
    mov [user_ctx + 40], r14
    mov [user_ctx + 48], r15
    mov rax, [rsp]              ; return address
    mov [user_ctx + 56], rax
    mov rsp, rcx                ; kstack_top (arg 4)
    mov cr3, rdx                ; new address space (arg 3)
    call usermode_enter         ; rdi=entry, rsi=user_rsp; exit unwinds below

; void user_ctx_restore(void) - unwind to the saved kernel context.
; The exit path arrives here from the int 0x80 gate, which cleared IF and never
; ran an iretq, so interrupts must be re-enabled explicitly.
user_ctx_restore:
    sti
    mov rsp, [user_ctx + 0]
    mov rbx, [user_ctx + 8]
    mov rbp, [user_ctx + 16]
    mov r12, [user_ctx + 24]
    mov r13, [user_ctx + 32]
    mov r14, [user_ctx + 40]
    mov r15, [user_ctx + 48]
    mov rax, [user_ctx + 56]
    add rsp, 8                  ; drop the return-address slot the call pushed
    jmp rax
