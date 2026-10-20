/* int pal_guarded_copy_asm(void *dst, const void *src, size_t n)   (Win64 ABI: rcx, rdx, r8)
   Returns 0, or 1 when a page faults during the copy: the vectored handler in pal_win.c sees the fault at
   pal_guarded_copy_insn and resumes at pal_guarded_copy_fault. rdi and rsi are callee-saved on Win64. */
    .intel_syntax noprefix
    .text
    .globl pal_guarded_copy_asm
    .globl pal_guarded_copy_insn
    .globl pal_guarded_copy_fault
    .p2align 4
pal_guarded_copy_asm:
    push rdi
    push rsi
    mov rdi, rcx
    mov rsi, rdx
    mov rcx, r8
pal_guarded_copy_insn:
    rep movsb
    xor eax, eax
1:
    pop rsi
    pop rdi
    ret
pal_guarded_copy_fault:
    mov eax, 1
    jmp 1b
