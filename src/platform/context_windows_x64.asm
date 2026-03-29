; x86-64 Windows context switch implementation
; Microsoft x64 calling convention: rcx = from, rdx = to

.code

; Context layout:
; offset 0-127:   gp_regs[16] (128 bytes)
; offset 128-287: xmm_regs[160] (160 bytes)
; offset 288:     stack_ptr (8 bytes)
; offset 296:     return_addr (8 bytes)

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

    ; Save xmm6-xmm15 (non-volatile on Windows)
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

    ; Save return address and stack pointer
    mov     rax, [rsp]              ; Get return address
    lea     r11, [rsp + 8]          ; rsp after return
    mov     [rcx + 288], r11        ; Save rsp
    mov     [rcx + 296], rax        ; Save return address

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

    ; Load return address and jump
    mov     rax, [rdx + 296]
    jmp     rax
SwapContext ENDP

MakeContext PROC
    ; MakeContext(ctx, stack_top, stack_size, fn, arg)
    ; rcx=ctx, rdx=stack_top (highest address), r8=stack_size (UNUSED), r9=fn, [rsp+40]=arg
    ;
    ; Note: stack_top is already the highest address from AllocateStack

    mov     r10, [rsp + 40]  ; Load arg from stack

    ; stack_top (rdx) is already 16-byte aligned by AllocateStack
    mov     rax, rdx

    ; Stack layout for BthreadStart (must be 16-byte aligned):
    ; We set up stack so that when BthreadStart runs:
    ;   rsp+8  is 16-byte aligned (for proper function call alignment)
    ;   [rsp+8]  = fn
    ;   [rsp+16] = arg
    ;
    ; sub 32 gives: rsp aligned to 16, and rsp+8 is also aligned to 16
    sub     rax, 32

    ; Store fn at [rsp+8]
    mov     [rax + 8], r9

    ; Store arg at [rsp+16]
    mov     [rax + 16], r10

    ; Set stack pointer
    mov     [rcx + 288], rax

    ; Set return address to BthreadStart
    lea     rax, BthreadStart
    mov     [rcx + 296], rax

    ret
MakeContext ENDP

; Entry point for new bthreads
; Stack layout when we enter:
; [rsp]     = (unused, for alignment)
; [rsp+8]   = fn address
; [rsp+16]  = arg
;
; After we adjust: rsp+8 will be 16-byte aligned
BthreadStart PROC
    ; Load arg into rcx (first argument in Windows x64)
    mov     rcx, [rsp + 16]

    ; Load function address
    mov     rax, [rsp + 8]

    ; Adjust stack pointer up by 8 to align
    ; Now [rsp] is the old [rsp+8] = fn
    ; And stack is properly aligned for the function call
    add     rsp, 8

    ; We're going to jmp to fn, not call it
    ; The function will receive: rcx = arg
    ; Stack alignment should be correct now
    jmp     rax
BthreadStart ENDP

END