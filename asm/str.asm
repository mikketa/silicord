; String primitives. Win64 ABI: args in rcx, rdx, r8, r9; result in rax.
default rel
bits 64

section .text

; size_t sc_strlen(const char *s)
;
; SSE2, 16 bytes per step. Every load is an aligned 16-byte block, and a block
; never straddles a page, so reading past the terminator within its block is
; safe; bytes of the first block before `s` are shifted out of the mask.
global sc_strlen
sc_strlen:
    mov     rax, rcx
    mov     rdx, rcx
    and     rdx, -16                ; the block holding s[0]
    pxor    xmm0, xmm0
    movdqa  xmm1, [rdx]
    pcmpeqb xmm1, xmm0
    pmovmskb r8d, xmm1              ; bit i: byte i of the block is 0
    and     ecx, 15
    shr     r8d, cl                 ; drop the bytes before s
    test    r8d, r8d
    jnz     .first
.loop:
    add     rdx, 16
    movdqa  xmm1, [rdx]
    pcmpeqb xmm1, xmm0
    pmovmskb r8d, xmm1
    test    r8d, r8d
    jz      .loop
    bsf     r8d, r8d
    add     rdx, r8
    sub     rdx, rax
    mov     rax, rdx
    ret
.first:
    bsf     eax, r8d                ; the terminator is in the first block
    ret
