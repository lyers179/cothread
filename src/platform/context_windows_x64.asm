; x86-64 Windows context switch implementation
; Microsoft x64 calling convention: rcx = from, rdx = to

.code

SwapContext PROC
    ; Save non-volatile GPRs (rbx, rbp, rsi, rdi, r12-r15)
    mov     [rcx + 0*8], rbx
    mov     [rcx + 1*8], rbp
    mov     [rcx + 2*8], rsi
    mov     [rcx + 3*8], rdi
    mov     [rcx + 4*8], r12
    mov     [rcx + 5*8], r13
    mov     [rcx + 6*8], r14
    mov     [rcx + 7*8], r15

    ; Save xmm6-xmm15
    movdqa  xmmword ptr [rcx + 128], xmm6
    movdqa  xmmword ptr [rcx + 144], xmm7
    movdqa  xmmword ptr [rcx + 160], xmm8
    movdqa  xmmword ptr [rcx + 176], xmm9
    movdqa  xmmword ptr [rcx + 192], xmm10
    movdqa  xmmword ptr [rcx + 208], xmm11
    movdqa  xmmword ptr [rcx + 224], xmm12
    movdqa  xmmword ptr [rcx + 240], xmm13
    movdqa  xmmword ptr [rcx + 256], xmm14
    movdqa  xmmword ptr [rcx + 272], xmm15

    ; Save stack pointer (offset 288 = 16*8 + 160)
    mov     [rcx + 288], rsp

    ; Load non-volatile GPRs from 'to'
    mov     rbx, [rdx + 0*8]
    mov     rbp, [rdx + 1*8]
    mov     rsi, [rdx + 2*8]
    mov     rdi, [rdx + 3*8]
    mov     r12, [rdx + 4*8]
    mov     r13, [rdx + 5*8]
    mov     r14, [rdx + 6*8]
    mov     r15, [rdx + 7*8]

    ; Load xmm6-xmm15
    movdqa  xmm6, xmmword ptr [rdx + 128]
    movdqa  xmm7, xmmword ptr [rdx + 144]
    movdqa  xmm8, xmmword ptr [rdx + 160]
    movdqa  xmm9, xmmword ptr [rdx + 176]
    movdqa  xmm10, xmmword ptr [rdx + 192]
    movdqa  xmm11, xmmword ptr [rdx + 208]
    movdqa  xmm12, xmmword ptr [rdx + 224]
    movdqa  xmm13, xmmword ptr [rdx + 240]
    movdqa  xmm14, xmmword ptr [rdx + 256]
    movdqa  xmm15, xmmword ptr [rdx + 272]

    ; Load stack pointer
    mov     rsp, [rdx + 288]

    ; Return to new context
    ret
SwapContext ENDP

MakeContext PROC
    ; MakeContext(ctx, stack, stack_size, fn, arg)
    ; rcx=ctx, rdx=stack, r8=stack_size, r9=fn, [rsp+40]=arg

    mov     r10, [rsp + 40]  ; Load arg from stack

    ; Calculate stack top (highest address, 16-byte aligned)
    mov     rax, rdx
    add     rax, r8
    and     rax, -16

    ; Reserve space for return address and align stack
    sub     rax, 32

    ; Store fn as return address
    mov     [rax + 24], r9

    ; Set stack pointer
    mov     [rcx + 288], rax

    ; Set first argument (will be in rcx when fn starts)
    mov     qword ptr [rcx + 0*8], r10

    ret
MakeContext ENDP

END