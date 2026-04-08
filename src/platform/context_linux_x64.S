// x86-64 Linux context switch implementation
// System V ABI calling convention: rdi = from, rsi = to
//
// Context layout (matches Windows):
// offset 0-127:   gp_regs[16] (128 bytes)
// offset 128-287: xmm_regs[160] (160 bytes) - not saved on Linux (caller-saved in SysV ABI)
// offset 288:     stack_ptr (8 bytes)
// offset 296:     return_addr (8 bytes)

.global SwapContext
.type SwapContext, @function
SwapContext:
    // Save callee-saved registers (rbx, rbp, r12-r15) - System V ABI
    // Store at gp_regs offsets 0, 8, 16, 24, 32, 40
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)

    // Save return address and stack pointer
    movq    (%rsp), %rax           // Get return address
    leaq    8(%rsp), %r11          // rsp after return
    movq    %r11, 288(%rdi)        // Save stack_ptr at offset 288
    movq    %rax, 296(%rdi)        // Save return_addr at offset 296

    // Load callee-saved registers from 'to'
    movq    0(%rsi), %rbx
    movq    8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15

    // Load stack pointer
    movq    288(%rsi), %rsp

    // Load return address and jump
    movq    296(%rsi), %rax
    jmp     *%rax
.size SwapContext, .-SwapContext

.global MakeContext
.type MakeContext, @function
// MakeContext(ctx, stack_top, stack_size, fn, arg)
// rdi=ctx, rsi=stack_top (highest address), rdx=stack_size (unused), rcx=fn, r8=arg
MakeContext:
    // stack_top (rsi) is already 16-byte aligned
    movq    %rsi, %rax

    // Stack layout for BthreadStart (must be 16-byte aligned):
    // After return, rsp+8 should be 16-byte aligned
    // [rsp+8]  = fn
    // [rsp+16] = arg
    subq    $32, %rax

    // Store fn at [rsp+8]
    movq    %rcx, 8(%rax)

    // Store arg at [rsp+16]
    movq    %r8, 16(%rax)

    // Set stack pointer at offset 288
    movq    %rax, 288(%rdi)

    // Set return address to BthreadStart at offset 296
    leaq    BthreadStart(%rip), %rax
    movq    %rax, 296(%rdi)

    ret
.size MakeContext, .-MakeContext

// Entry point for new bthreads
.global BthreadStart
.type BthreadStart, @function
BthreadStart:
    // Stack layout when we enter:
    // [rsp]     = (unused, for alignment)
    // [rsp+8]   = fn address
    // [rsp+16]  = arg
    //
    // System V ABI: first argument in rdi
    movq    16(%rsp), %rdi         // Load arg into rdi
    movq    8(%rsp), %rax          // Load function address
    addq    $8, %rsp               // Adjust stack (now properly aligned)
    jmp     *%rax                  // Jump to fn(rdi=arg)
.size BthreadStart, .-BthreadStart

// Mark stack as non-executable (required by modern Linux)
.section .note.GNU-stack,"",@progbits