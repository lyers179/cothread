// x86-64 Windows context switch implementation for GCC/MinGW
// GAS assembler with Intel syntax mode
// Microsoft x64 calling convention: rcx = from, rdx = to, r8 = to_uses_xmm

.intel_syntax noprefix

// Context layout:
// offset 0-127:   gp_regs[16] (128 bytes)
// offset 128-287: xmm_regs[160] (160 bytes)
// offset 288:     stack_ptr (8 bytes)
// offset 296:     return_addr (8 bytes)

.global SwapContext
.global MakeContext
.global BthreadStart

.text

// SwapContext(from, to, to_uses_xmm_ptr)
// rcx = from, rdx = to, r8 = to_uses_xmm_ptr

SwapContext:
    // ============== Save XMM (if needed) ==============
    test    r8, r8
    jz      save_xmm_zero_detect

    // Has uses_xmm pointer, check flag
    mov     r9, [r8]
    test    r9, r9
    jnz     save_xmm_now

    // First time: detect if xmm6-xmm15 are non-zero
    // Use xmm0 (volatile) for accumulation
    movdqa  xmm0, xmm6
    por     xmm0, xmm7
    por     xmm0, xmm8
    por     xmm0, xmm9
    por     xmm0, xmm10
    por     xmm0, xmm11
    por     xmm0, xmm12
    por     xmm0, xmm13
    por     xmm0, xmm14
    por     xmm0, xmm15

    ptest   xmm0, xmm0
    jz      save_xmm_skip

    // At least one xmm is non-zero, set uses_xmm
    mov     byte ptr [r8], 1

save_xmm_now:
    // Save xmm6-xmm15
    movdqa  [rcx + 128], xmm6
    movdqa  [rcx + 144], xmm7
    movdqa  [rcx + 160], xmm8
    movdqa  [rcx + 176], xmm9
    movdqa  [rcx + 192], xmm10
    movdqa  [rcx + 208], xmm11
    movdqa  [rcx + 224], xmm12
    movdqa  [rcx + 240], xmm13
    movdqa  [rcx + 256], xmm14
    movdqa  [rcx + 272], xmm15
    jmp     save_xmm_done

save_xmm_zero_detect:
    // No uses_xmm pointer, save always (backward compatible)
    movdqa  [rcx + 128], xmm6
    movdqa  [rcx + 144], xmm7
    movdqa  [rcx + 160], xmm8
    movdqa  [rcx + 176], xmm9
    movdqa  [rcx + 192], xmm10
    movdqa  [rcx + 208], xmm11
    movdqa  [rcx + 224], xmm12
    movdqa  [rcx + 240], xmm13
    movdqa  [rcx + 256], xmm14
    movdqa  [rcx + 272], xmm15

save_xmm_skip:
save_xmm_done:
    // ============== Save GPRs ==============
    mov     [rcx + 0*8], rbx
    mov     [rcx + 1*8], rbp
    mov     [rcx + 2*8], rsi
    mov     [rcx + 3*8], rdi
    mov     [rcx + 4*8], r12
    mov     [rcx + 5*8], r13
    mov     [rcx + 6*8], r14
    mov     [rcx + 7*8], r15

    // Save return address and stack pointer
    mov     rax, [rsp]
    lea     r11, [rsp + 8]
    mov     [rcx + 288], r11
    mov     [rcx + 296], rax

    // ============== Load GPRs ==============
    mov     rbx, [rdx + 0*8]
    mov     rbp, [rdx + 1*8]
    mov     rsi, [rdx + 2*8]
    mov     rdi, [rdx + 3*8]
    mov     r12, [rdx + 4*8]
    mov     r13, [rdx + 5*8]
    mov     r14, [rdx + 6*8]
    mov     r15, [rdx + 7*8]

    // ============== Load XMM (if needed) ==============
    test    r8, r8
    jz      load_xmm_always

    mov     r9, [r8]
    test    r9, r9
    jz      load_xmm_skip

load_xmm_now:
    // Load xmm6-xmm15
    movdqa  xmm6, [rdx + 128]
    movdqa  xmm7, [rdx + 144]
    movdqa  xmm8, [rdx + 160]
    movdqa  xmm9, [rdx + 176]
    movdqa  xmm10, [rdx + 192]
    movdqa  xmm11, [rdx + 208]
    movdqa  xmm12, [rdx + 224]
    movdqa  xmm13, [rdx + 240]
    movdqa  xmm14, [rdx + 256]
    movdqa  xmm15, [rdx + 272]
    jmp     load_xmm_done

load_xmm_skip:
load_xmm_always:
    // Backward compatible: always load if no uses_xmm
    movdqa  xmm6, [rdx + 128]
    movdqa  xmm7, [rdx + 144]
    movdqa  xmm8, [rdx + 160]
    movdqa  xmm9, [rdx + 176]
    movdqa  xmm10, [rdx + 192]
    movdqa  xmm11, [rdx + 208]
    movdqa  xmm12, [rdx + 224]
    movdqa  xmm13, [rdx + 240]
    movdqa  xmm14, [rdx + 256]
    movdqa  xmm15, [rdx + 272]

load_xmm_done:
    // Load stack pointer
    mov     rsp, [rdx + 288]

    // Load return address and jump
    mov     rax, [rdx + 296]
    jmp     rax

// MakeContext(ctx, stack_top, stack_size, fn, arg)
// rcx=ctx, rdx=stack_top, r8=stack_size, r9=fn, [rsp+40]=arg
MakeContext:
    mov     r10, [rsp + 40]  // Load arg from stack

    // Save fn to r11 (volatile) since we need r9 for zeroing
    mov     r11, r9

    // Zero xmm region in context for runtime detection
    lea     r9, [rcx + 128]   // xmm_regs offset
    mov     rax, 20           // 160 bytes / 8 = 20 qwords
    xor     r8, r8            // Zero out r8 (r8 is stack_size, we're done with it)
zero_xmm_loop:
    mov     [r9], r8
    add     r9, 8
    dec     rax
    jnz     zero_xmm_loop

    // Stack setup
    lea     rax, [rdx]
    sub     rax, 32

    mov     [rax + 8], r11     // fn from saved r11
    mov     [rax + 16], r10

    mov     [rcx + 288], rax

    lea     rax, [rip + BthreadStart]
    mov     [rcx + 296], rax

    ret

// Entry point for new bthreads
BthreadStart:
    mov     rcx, [rsp + 16]

    mov     rax, [rsp + 8]

    add     rsp, 8

    jmp     rax