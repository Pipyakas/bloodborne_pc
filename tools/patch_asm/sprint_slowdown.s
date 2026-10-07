# Frame-rate-independent sprint "stuck" check for Bloodborne 1.09 (eboot vaddr 0x1514b92-0x1514c95,
# PS4 address 0x1914b92). Replaces the tail of the character movement update 0x1514500, which the
# FPS presets leave at the 30 FPS tuning: when the requested speed is above 5.0 and the character
# moved less than 1/30 unit this frame (distance * 30 < 1), the speed multiplier [r13+0x1e0] is
# cut by 0.8 per frame (floor 0.5), otherwise it recovers by 1.2 per frame (cap 1.0). At 120 FPS
# a normal sprint after a bump stays under the per-frame distance and the slowdown latches.
# Here: stuck when distance / dt < 1 (dt = the function's frame time argument, [rbp-0x74]), and the
# factors are 0.8^(30 dt) and 1.2^(30 dt) (Pade (2,2) exp: relative error < 1e-5 up to 30 dt = 3, positive and finite for any dt).
# At dt = 1/30 this is the original behaviour. Assemble: tools/patch_asm/build.sh.
        .intel_syntax noprefix
        .set BASE, 0x1514b92
        .set FLOOR, 0x1514cc2            # mov dword [r13+0x1e0], 0.5; jmp DONE
        .set DONE, 0x1514c95             # stack check and epilogue
        .set ONE, 0x4926318              # the game's constants: 1.0, 30.0, 0.5
        .set C30, 0x492632c
        .set C05, 0x4926330
        .text
start:
        mov     rax, [r13 + 8]
        mov     rax, [rax + 0x3b0]
        mov     rax, [rax + 0x68]
        vmovaps xmm0, [rax + 0x1e0]
        vsubps  xmm0, xmm0, [rax + 0x1f0]
        vdpps   xmm0, xmm0, xmm0, 0x71
        vsqrtss xmm0, xmm0, xmm0
        vmulss  xmm0, xmm0, [rip + start + (C30 - BASE)]  # distance * 30
        call    steps                    # xmm2 = 30 dt (0 when dt is negative or NaN)
        vucomiss xmm2, xmm0
        jbe     recover
        vmovss  xmm0, [r13 + 0x1e0]
        vucomiss xmm0, [rip + start + (C05 - BASE)]
        jbe     start + (FLOOR - BASE)
        vmovss  xmm1, [rip + ln08]
scale:                                   # [r13+0x1e0] = xmm0 * exp(xmm1 * xmm2)
        vmulss  xmm1, xmm1, xmm2
        vmulss  xmm3, xmm1, xmm1
        vaddss  xmm3, xmm3, [rip + c12]
        vmulss  xmm1, xmm1, [rip + c6]
        vaddss  xmm4, xmm3, xmm1
        vsubss  xmm3, xmm3, xmm1
        vdivss  xmm1, xmm4, xmm3
        vmulss  xmm0, xmm0, xmm1
        vmovss  [r13 + 0x1e0], xmm0
        jmp     start + (DONE - BASE)
one:
        mov     dword ptr [r13 + 0x1e0], 0x3f800000
        jmp     start + (DONE - BASE)
steps:
        vmovss  xmm2, [rbp - 0x74]       # the function's frame time argument
        vmulss  xmm2, xmm2, [rip + start + (C30 - BASE)]
        vxorps  xmm3, xmm3, xmm3
        vmaxss  xmm2, xmm2, xmm3
        ret
c6:     .float 6.0
c12:    .float 12.0
ln08:   .float -0.22314355               # ln 0.8
ln12:   .float 0.18232156                # ln 1.2
        .org 0x1514c60 - BASE, 0xcc      # the two outside branches enter the recovery here
recover:
        vmovss  xmm0, [r13 + 0x1e0]
        vmovss  xmm1, [rip + start + (ONE - BASE)]
        vucomiss xmm1, xmm0
        jbe     one
        call    steps
        vmovss  xmm1, [rip + ln12]
        jmp     scale
        .org DONE - BASE, 0xcc
