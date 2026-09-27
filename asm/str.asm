; String primitives. Win64 ABI: args in rcx, rdx, r8, r9; result in rax.
default rel
bits 64

section .text

; size_t sc_strlen(const char *s)
global sc_strlen
sc_strlen:
    mov     rax, rcx
.loop:
    cmp     byte [rax], 0
    je      .done
    inc     rax
    jmp     .loop
.done:
    sub     rax, rcx
    ret
