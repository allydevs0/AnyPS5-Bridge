/* Register trampoline for the AVX2 stub differential test.
 *
 * The stub under test is raw code that has to be entered with an arbitrary
 * register file, which no C calling convention can express. This function loads
 * all 16 ymm registers and all 15 usable GPRs from an input block, runs the stub
 * on a private canary stack, and writes the full register file back.
 *
 *   void avx2_diff_run(const RegFile* in, RegFile* out, const void* code, void* scratch)
 *     rcx = in, rdx = out, r8 = code, r9 = scratch
 *
 * `scratch` is a 16-byte-aligned buffer of at least 64 KiB. The caller hands
 * over base + kStackTop so there is 4 KiB of headroom *underneath* for the
 * stub's frame (the stub grows the stack downwards from the value it is given),
 * and so that rsp is 16-byte aligned at the call.
 *
 * out/code/original-rsp are parked just *above* the stub's entry rsp, not below
 * it. The stub reaches down: it pushes its base register and then sets its frame
 * base with `lea base, [rsp-128]`, so its frame occupies [R-272, R-16) and it may
 * also `sub rsp` further. Anything parked below R is therefore inside the region
 * the stub writes and gets destroyed -- an `out` pointer overwritten with frame
 * bytes showed up as a fault on the very first vmovdqu in avx2_diff_cont.
 * Above R the stub never writes, so the stash is safe.
 *
 * The stub ends in `jmp rel32` rather than `ret`, so the return address `callq`
 * pushed is still on the stack on arrival; it is dropped before the slots are read.
 *
 * The stub ends in `ret`, so the stub's ret returns here and r11 survives
 * untouched -- expecting it to still hold `code` is what proves the stub's
 * push/pop frame is balanced.
 */

    .text


    .globl avx2_diff_run
    .def   avx2_diff_run; .scl 2; .type 32; .endef
avx2_diff_run:
    pushq  %rbx
    pushq  %rbp
    pushq  %rsi
    pushq  %rdi
    pushq  %r12
    movq   %rcx, %r11                   # in
    movq   %r9, %r12                    # R: stack top handed to the stub

    # Stash the three values the stub may clobber, on the canary stack and addressed
    # purely off rsp. No base register is used: every GPR here is overwritten with the
    # guest's value before the stub runs, which is the whole point of the test, so a
    # register could not hold the pointer across the call.
    #
    # `original rsp` is captured BEFORE rsp is switched, because it is this
    # trampoline's frame pointer -- restoring it is what makes the trailing pops and
    # the final ret unwind correctly.
    #
    # The slots sit at R-0x100.. The stub reaches further down than that: it pushes
    # its base register and then sets its frame base with `lea base,[rsp-128]`, which
    # puts the frame at [R-272, R-136). Parking the slots inside that window let frame
    # traffic overwrite `out`, which showed up as a fault on the first store back.
    movq   %rsp, %rax
    movq   %rax, -0x300(%r12)           # original rsp
    movq   %rdx, %rax
    movq   %rax, -0x308(%r12)           # out
    movq   %r8, %rax
    movq   %rax, -0x310(%r12)           # code: r8 gets overwritten by the guest's r8
    movq   %r12, %rsp                   # run on the canary stack

    vmovdqu 0x000(%r11), %ymm0
    vmovdqu 0x020(%r11), %ymm1
    vmovdqu 0x040(%r11), %ymm2
    vmovdqu 0x060(%r11), %ymm3
    vmovdqu 0x080(%r11), %ymm4
    vmovdqu 0x0a0(%r11), %ymm5
    vmovdqu 0x0c0(%r11), %ymm6
    vmovdqu 0x0e0(%r11), %ymm7
    vmovdqu 0x100(%r11), %ymm8
    vmovdqu 0x120(%r11), %ymm9
    vmovdqu 0x140(%r11), %ymm10
    vmovdqu 0x160(%r11), %ymm11
    vmovdqu 0x180(%r11), %ymm12
    vmovdqu 0x1a0(%r11), %ymm13
    vmovdqu 0x1c0(%r11), %ymm14
    vmovdqu 0x1e0(%r11), %ymm15

    movq   0x200(%r11), %rax
    movq   0x208(%r11), %rcx
    movq   0x210(%r11), %rdx
    movq   0x218(%r11), %rbx
    movq   0x228(%r11), %rbp
    movq   0x230(%r11), %rsi
    movq   0x238(%r11), %rdi
    movq   0x240(%r11), %r8
    movq   0x248(%r11), %r9
    movq   0x250(%r11), %r10
    movq   0x260(%r11), %r12
    movq   0x268(%r11), %r13
    movq   0x270(%r11), %r14
    movq   0x278(%r11), %r15

    # r12 no longer holds R: the line above loaded the guest's r12 over it. Re-derive
    # it from the stack instead, which the stub has not touched yet (it still points
    # at the trampoline frame, since `movq %r12, %rsp` has not run).
    movq   -0x310(%rsp), %r11           # r11 = code
    callq  *%r11                         # rsp = R-8

    .globl avx2_diff_cont
    .def   avx2_diff_cont; .scl 2; .type 32; .endef
avx2_diff_cont:                          # rsp = R-8: the stub tail is a JMP
    # The generated stubs end in `jmp rel32`, not `ret`, so the return address
    # `callq` pushed is still on the stack when we get here. Drop it so rsp is R
    # again and every stack-relative slot below lines up. (A `ret`-terminated
    # stub would already have popped it, so the selftest stub is jmp-terminated
    # too -- production stubs and the selftest must agree on this.)
    addq   $8, %rsp

    movq   %r10, -0x10(%rsp)            # park the stub's final r10

    movq   -0x308(%rsp), %r10           # out
    vmovdqu %ymm0,  0x000(%r10)
    vmovdqu %ymm1,  0x020(%r10)
    vmovdqu %ymm2,  0x040(%r10)
    vmovdqu %ymm3,  0x060(%r10)
    vmovdqu %ymm4,  0x080(%r10)
    vmovdqu %ymm5,  0x0a0(%r10)
    vmovdqu %ymm6,  0x0c0(%r10)
    vmovdqu %ymm7,  0x0e0(%r10)
    vmovdqu %ymm8,  0x100(%r10)
    vmovdqu %ymm9,  0x120(%r10)
    vmovdqu %ymm10, 0x140(%r10)
    vmovdqu %ymm11, 0x160(%r10)
    vmovdqu %ymm12, 0x180(%r10)
    vmovdqu %ymm13, 0x1a0(%r10)
    vmovdqu %ymm14, 0x1c0(%r10)
    vmovdqu %ymm15, 0x1e0(%r10)

    movq   %rax, 0x200(%r10)
    movq   %rcx, 0x208(%r10)
    movq   %rdx, 0x210(%r10)
    movq   %rbx, 0x218(%r10)
    movq   %rbp, 0x228(%r10)
    movq   %rsi, 0x230(%r10)
    movq   %rdi, 0x238(%r10)
    movq   %r8,  0x240(%r10)
    movq   %r9,  0x248(%r10)
    movq   %r12, 0x260(%r10)
    movq   %r13, 0x268(%r10)
    movq   %r14, 0x270(%r10)
    movq   %r15, 0x278(%r10)
    movq   -0x10(%rsp), %rcx            # rcx = stub's final r10
    movq   %rcx, 0x250(%r10)
    movq   %r11, 0x258(%r10)

    movq   -0x300(%rsp), %r11           # original rsp
    movq   %r11, %rsp
    popq   %r12
    popq   %rdi
    popq   %rsi
    popq   %rbp
    popq   %rbx
    ret
