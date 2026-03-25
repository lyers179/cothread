// x86-64 Linux context switch implementation
// System V ABI calling convention: rdi = from, rsi = to

.global SwapContext
.type SwapContext, @function
SwapContext:
    // Save callee-saved registers (rbx, rbp, r12-r15)
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)

    // Save stack pointer (offset 112 = 14*8)
    movq    %rsp, 112(%rdi)

    // Load callee-saved registers from 'to'
    movq    0(%rsi), %rbx
    movq    8(%rsi), %rbp
    movq    16(%rsi), %r12
    movq    24(%rsi), %r13
    movq    32(%rsi), %r14
    movq    40(%rsi), %r15

    // Load stack pointer
    movq    112(%rsi), %rsp

    // Return to new context
    ret
.size SwapContext, .-SwapContext

.global MakeContext
.type MakeContext, @function
// MakeContext(ctx, stack, stack_size, fn, arg)
// rdi=ctx, rsi=stack, rdx=stack_size, rcx=fn, r8=arg
MakeContext:
    // Calculate stack top (highest address, 16-byte aligned)
    movq    %rsi, %rax
    addq    %rdx, %rax
    andq    $-16, %rax

    // Reserve space for return address
    subq    $8, %rax

    // Store fn as return address
    movq    %rcx, (%rax)

    // Set stack pointer
    movq    %rax, 112(%rdi)

    // Set first argument (arg will be in rdi when fn starts)
    movq    %r8, 0(%rdi)

    // Set return address (will never return through this)
    movq    $0, 120(%rdi)

    ret
.size MakeContext, .-MakeContext