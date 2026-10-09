@ ============================================================================
@ seal_core.S -- hand-written Thumb-1 (Cortex-M0+) core of the light FEC wrap.
@
@ Byte math over GF(256) with table lookups (seal_tables.S), 32 check bytes,
@ shortened to 120 data + 32 check = 152 byte codewords. Corrects up to 16
@ byte errors per codeword. Algorithm identical to fec_asm_tests/fec_model.py
@ (cross-checked by test_seal_asm.py under an emulator).
@
@   void seal_encode(const uint8_t data[120], uint8_t chk[33]);
@        chk must be 4-byte aligned, 33 bytes (chk[32] is scratch, left 0).
@   uint32_t seal_syndromes(const uint8_t cw[152], uint8_t syn[32]);
@        syn 4-byte aligned. Returns 0 iff all 32 syndromes are zero.
@   int32_t seal_decode(uint8_t cw[152]);
@        Corrects in place. Returns number of corrected bytes (0..16) or -1.
@
@ All routines follow the AAPCS (r4-r7 preserved, r0-r3/r12 scratch).
@ ============================================================================
    .syntax unified
    .cpu cortex-m0plus
    .thumb

@ work area ("ctx") layout, 240 bytes on the caller's stack, base in r7:
@   0 L   4 nroots   8 cw ptr   12 n/e   16 m/lx   20 b   24 d/num   (words)
@   32 S[32]  64 C[36]  100 B[36]  136 T[36]  172 OM[16]  188 RT[16]  204 LT[17 halfwords]
    .macro CTXP reg, off
    mov     \reg, r7
    adds    \reg, #\off
    .endm

    .section .text.seal_core,"ax",%progbits
    .align 2

@ ----------------------------------------------------------------------------
@ seal_encode
@ ----------------------------------------------------------------------------
    .global seal_encode
    .thumb_func
seal_encode:
    push    {r4-r7, lr}
    sub     sp, #8
    movs    r2, #120
    adds    r2, r0, r2              @ end of data
    str     r2, [sp]
    movs    r2, #0                  @ zero chk[0..32]
    str     r2, [r1, #0]
    str     r2, [r1, #4]
    str     r2, [r1, #8]
    str     r2, [r1, #12]
    str     r2, [r1, #16]
    str     r2, [r1, #20]
    str     r2, [r1, #24]
    str     r2, [r1, #28]
    movs    r3, #32
    strb    r2, [r1, r3]
    ldr     r4, =seal_exp
    ldr     r5, =seal_gl
1:                                  @ ---- per data byte ----
    ldrb    r2, [r0]
    adds    r0, r0, #1
    ldrb    r3, [r1]
    eors    r2, r3                  @ fb = d ^ chk[0]
    beq     4f                      @ fb == 0 -> plain shift
    lsls    r2, r2, #1
    ldr     r3, =seal_log
    ldrh    r2, [r3, r2]            @ r2 = log(fb)
    movs    r7, #4
2:                                  @ ---- 4 groups of 8 taps ----
    ldrb    r3, [r5, #0]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #1]
    eors    r3, r6
    strb    r3, [r1, #0]

    ldrb    r3, [r5, #1]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #2]
    eors    r3, r6
    strb    r3, [r1, #1]

    ldrb    r3, [r5, #2]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #3]
    eors    r3, r6
    strb    r3, [r1, #2]

    ldrb    r3, [r5, #3]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #4]
    eors    r3, r6
    strb    r3, [r1, #3]

    ldrb    r3, [r5, #4]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #5]
    eors    r3, r6
    strb    r3, [r1, #4]

    ldrb    r3, [r5, #5]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #6]
    eors    r3, r6
    strb    r3, [r1, #5]

    ldrb    r3, [r5, #6]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #7]
    eors    r3, r6
    strb    r3, [r1, #6]

    ldrb    r3, [r5, #7]
    adds    r3, r3, r2
    ldrb    r3, [r4, r3]
    ldrb    r6, [r1, #8]
    eors    r3, r6
    strb    r3, [r1, #7]

    adds    r1, #8
    adds    r5, #8
    subs    r7, #1
    bne     2b
    subs    r1, #32
    subs    r5, #32
3:                                  @ ---- next data byte ----
    ldr     r3, [sp]
    cmp     r0, r3
    bne     1b
    add     sp, #8
    pop     {r4-r7, pc}
4:                                  @ fb == 0: chk = chk << 1 (bytes)
    movs    r7, #32
5:
    ldrb    r3, [r1, #1]
    strb    r3, [r1, #0]
    adds    r1, #1
    subs    r7, #1
    bne     5b
    subs    r1, #32
    b       3b
    .ltorg

@ ----------------------------------------------------------------------------
@ seal_syndromes -- S[j] = cw(alpha^j), j = 0..31, by Horner over all 152 bytes
@ ----------------------------------------------------------------------------
    .global seal_syndromes
    .thumb_func
seal_syndromes:
    push    {r4-r7, lr}
    sub     sp, #8
    movs    r2, #152
    adds    r2, r0, r2
    str     r2, [sp]
    movs    r2, #0
    str     r2, [r1, #0]
    str     r2, [r1, #4]
    str     r2, [r1, #8]
    str     r2, [r1, #12]
    str     r2, [r1, #16]
    str     r2, [r1, #20]
    str     r2, [r1, #24]
    str     r2, [r1, #28]
    ldr     r5, =seal_log
1:                                  @ ---- per codeword byte ----
    ldrb    r6, [r0]
    adds    r0, r0, #1
    mov     r2, r1
    ldr     r4, =seal_exp
    movs    r7, #4
2:                                  @ ---- 4 groups of 8 syndromes ----
    ldrb    r3, [r2, #0]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #0
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #0]

    ldrb    r3, [r2, #1]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #1
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #1]

    ldrb    r3, [r2, #2]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #2
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #2]

    ldrb    r3, [r2, #3]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #3
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #3]

    ldrb    r3, [r2, #4]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #4
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #4]

    ldrb    r3, [r2, #5]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #5
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #5]

    ldrb    r3, [r2, #6]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #6
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #6]

    ldrb    r3, [r2, #7]
    lsls    r3, r3, #1
    ldrh    r3, [r5, r3]
    adds    r3, #7
    ldrb    r3, [r4, r3]
    eors    r3, r6
    strb    r3, [r2, #7]

    adds    r2, #8
    adds    r4, #8
    subs    r7, #1
    bne     2b
    ldr     r3, [sp]
    cmp     r0, r3
    bne     1b
    movs    r0, #0                  @ return OR of all syndromes
    ldr     r3, [r1, #0]
    orrs    r0, r3
    ldr     r3, [r1, #4]
    orrs    r0, r3
    ldr     r3, [r1, #8]
    orrs    r0, r3
    ldr     r3, [r1, #12]
    orrs    r0, r3
    ldr     r3, [r1, #16]
    orrs    r0, r3
    ldr     r3, [r1, #20]
    orrs    r0, r3
    ldr     r3, [r1, #24]
    orrs    r0, r3
    ldr     r3, [r1, #28]
    orrs    r0, r3
    add     sp, #8
    pop     {r4-r7, pc}
    .ltorg

@ ----------------------------------------------------------------------------
@ gf_mul: r0 = r0 * r1 (GF(256)). Clobbers r1-r3. Preserves r4-r7.
@ gf_div: r0 = r0 / r1 (r1 != 0). Clobbers r1-r3.
@ ----------------------------------------------------------------------------
    .thumb_func
gf_mul:
    cmp     r0, #0
    beq     1f
    cmp     r1, #0
    beq     2f
    ldr     r2, =seal_log
    lsls    r0, r0, #1
    ldrh    r0, [r2, r0]
    lsls    r1, r1, #1
    ldrh    r1, [r2, r1]
    adds    r0, r0, r1
    ldr     r2, =seal_exp
    ldrb    r0, [r2, r0]
1:
    bx      lr
2:
    movs    r0, #0
    bx      lr

    .thumb_func
gf_div:
    cmp     r0, #0
    beq     1f
    ldr     r2, =seal_log
    lsls    r0, r0, #1
    ldrh    r0, [r2, r0]
    lsls    r1, r1, #1
    ldrh    r1, [r2, r1]
    movs    r3, #255
    adds    r0, r0, r3
    subs    r0, r0, r1
    ldr     r2, =seal_exp
    ldrb    r0, [r2, r0]
1:
    bx      lr
    .ltorg

@ ----------------------------------------------------------------------------
@ seal_bm(ctx r0) -> r0 = L : error locator C(x) (ctx+64) from syndromes (ctx+32)
@ ----------------------------------------------------------------------------
    .thumb_func
seal_bm:
    push    {r4-r7, lr}
    mov     r7, r0
    movs    r1, #0                  @ zero C and B (64..135 = 18 words)
    CTXP    r0, 64
    movs    r2, #18
1:
    stmia   r0!, {r1}
    subs    r2, #1
    bne     1b
    movs    r1, #1
    CTXP    r0, 64
    strb    r1, [r0]                @ C[0] = 1
    CTXP    r0, 100
    strb    r1, [r0]                @ B[0] = 1
    movs    r1, #0
    str     r1, [r7, #0]            @ L = 0
    str     r1, [r7, #12]           @ n = 0
    movs    r1, #1
    str     r1, [r7, #16]           @ m = 1
    str     r1, [r7, #20]           @ b = 1
.Lbm_n:
    ldr     r2, [r7, #12]           @ n
    CTXP    r0, 32
    ldrb    r4, [r0, r2]            @ d = S[n]
    ldr     r5, [r7, #0]            @ L
    movs    r6, #1                  @ i
.Lbm_dot:
    cmp     r6, r5
    bhi     .Lbm_dotdone
    CTXP    r0, 64
    ldrb    r0, [r0, r6]            @ C[i]
    ldr     r1, [r7, #12]
    subs    r1, r1, r6
    CTXP    r2, 32
    ldrb    r1, [r2, r1]            @ S[n-i]
    bl      gf_mul
    eors    r4, r0
    adds    r6, #1
    b       .Lbm_dot
.Lbm_dotdone:
    str     r4, [r7, #24]           @ keep d
    cmp     r4, #0
    bne     .Lbm_nz
    ldr     r0, [r7, #16]
    adds    r0, #1
    str     r0, [r7, #16]           @ m++
    b       .Lbm_next
.Lbm_nz:
    mov     r0, r4
    ldr     r1, [r7, #20]
    bl      gf_div
    mov     r5, r0                  @ f = d / b
    ldr     r0, [r7, #0]
    lsls    r0, r0, #1
    ldr     r1, [r7, #12]
    cmp     r0, r1
    bhi     .Lbm_upd                @ 2L > n : no swap
    CTXP    r0, 64                  @ T = C
    CTXP    r1, 136
    movs    r2, #9
1:
    ldmia   r0!, {r3}
    stmia   r1!, {r3}
    subs    r2, #1
    bne     1b
.Lbm_upd:                           @ C[i+m] ^= f * B[i]
    movs    r6, #0
.Lbm_upd_l:
    ldr     r1, [r7, #16]
    adds    r2, r6, r1
    cmp     r2, #33
    bhi     .Lbm_upd_done
    CTXP    r0, 100
    ldrb    r1, [r0, r6]
    mov     r0, r5
    bl      gf_mul
    ldr     r1, [r7, #16]
    adds    r1, r1, r6
    CTXP    r2, 64
    ldrb    r3, [r2, r1]
    eors    r3, r0
    strb    r3, [r2, r1]
    adds    r6, #1
    b       .Lbm_upd_l
.Lbm_upd_done:
    ldr     r0, [r7, #0]
    lsls    r0, r0, #1
    ldr     r1, [r7, #12]
    cmp     r0, r1
    bhi     .Lbm_noswap
    ldr     r0, [r7, #12]           @ L = n + 1 - L
    adds    r0, #1
    ldr     r1, [r7, #0]
    subs    r0, r0, r1
    str     r0, [r7, #0]
    CTXP    r0, 136                 @ B = T
    CTXP    r1, 100
    movs    r2, #9
1:
    ldmia   r0!, {r3}
    stmia   r1!, {r3}
    subs    r2, #1
    bne     1b
    ldr     r0, [r7, #24]
    str     r0, [r7, #20]           @ b = d
    movs    r0, #1
    str     r0, [r7, #16]           @ m = 1
    b       .Lbm_next
.Lbm_noswap:
    ldr     r0, [r7, #16]
    adds    r0, #1
    str     r0, [r7, #16]           @ m++
.Lbm_next:
    ldr     r0, [r7, #12]
    adds    r0, #1
    str     r0, [r7, #12]
    cmp     r0, #32
    beq     .Lbm_end
    b       .Lbm_n
.Lbm_end:
    ldr     r0, [r7, #0]
    pop     {r4-r7, pc}
    .ltorg

@ ----------------------------------------------------------------------------
@ seal_chien(ctx r0) -> r0 = number of roots of C(x) among positions e=0..151
@ (17 if more than 16). Roots (exponents e) are stored at ctx+188.
@ ----------------------------------------------------------------------------
    .thumb_func
seal_chien:
    push    {r4-r7, lr}
    mov     r7, r0
    ldr     r5, =seal_log
    movs    r6, #0                  @ k
1:                                  @ LT[k] = log(C[k]) (512 if C[k] == 0)
    CTXP    r0, 64
    ldrb    r0, [r0, r6]
    lsls    r0, r0, #1
    ldrh    r0, [r5, r0]
    CTXP    r1, 204
    lsls    r2, r6, #1
    strh    r0, [r1, r2]
    adds    r6, #1
    cmp     r6, #17
    bne     1b
    movs    r0, #0
    str     r0, [r7, #4]            @ nroots = 0
    movs    r6, #0                  @ e
    ldr     r4, =seal_exp
2:                                  @ ---- per position: sum of 17 terms ----
    movs    r3, #0
    CTXP    r1, 204
    movs    r2, #32
3:
    ldrh    r0, [r1, r2]
    ldrb    r0, [r4, r0]
    eors    r3, r0
    subs    r2, #2
    bpl     3b
    cmp     r3, #0
    bne     5f
    ldr     r0, [r7, #4]            @ root found
    cmp     r0, #16
    bhs     9f
    CTXP    r1, 188
    strb    r6, [r1, r0]
    adds    r0, #1
    str     r0, [r7, #4]
5:                                  @ advance terms: LT[k] += 255 - k (mod 255)
    CTXP    r1, 204
    movs    r2, #1
6:
    lsls    r3, r2, #1
    ldrh    r0, [r1, r3]
    cmp     r0, #255
    bhs     7f                      @ zero-coefficient marker
    adds    r0, #255
    subs    r0, r0, r2
    cmp     r0, #255
    blo     8f
    subs    r0, #255
8:
    strh    r0, [r1, r3]
7:
    adds    r2, #1
    cmp     r2, #17
    bne     6b
    adds    r6, #1
    cmp     r6, #152
    bne     2b
    ldr     r0, [r7, #4]
    pop     {r4-r7, pc}
9:
    movs    r0, #17
    pop     {r4-r7, pc}
    .ltorg

@ ----------------------------------------------------------------------------
@ seal_forney(ctx r0) -> r0 = 0 ok / -1 : evaluates error magnitudes and
@ corrects the codeword (ctx+8 holds its address)
@ ----------------------------------------------------------------------------
    .thumb_func
seal_forney:
    push    {r4-r7, lr}
    mov     r7, r0
    movs    r6, #0                  @ k : OM[k] = sum_{i<=k} C[i] * S[k-i], k < L
1:
    ldr     r1, [r7, #0]
    cmp     r6, r1
    bhs     5f
    movs    r4, #0
    movs    r3, #0
2:
    CTXP    r0, 64
    ldrb    r0, [r0, r3]
    subs    r1, r6, r3
    CTXP    r2, 32
    ldrb    r1, [r2, r1]
    bl      gf_mul
    eors    r4, r0
    adds    r3, #1
    cmp     r3, r6
    bls     2b
    CTXP    r0, 172
    strb    r4, [r0, r6]
    adds    r6, #1
    b       1b
5:
    movs    r6, #0                  @ j : root index
.Lf_root:
    ldr     r0, [r7, #4]
    cmp     r6, r0
    bhs     .Lf_done
    CTXP    r0, 188
    ldrb    r0, [r0, r6]            @ e
    str     r0, [r7, #12]
    cmp     r0, #0                  @ lx = (255 - e) mod 255
    beq     1f
    movs    r1, #255
    subs    r0, r1, r0
1:
    str     r0, [r7, #16]
    movs    r4, #0                  @ ---- num = sum OM[k] * x^k ----
    movs    r3, #0
    movs    r5, #0
10:
    ldr     r1, [r7, #0]
    cmp     r5, r1
    bhs     12f
    CTXP    r0, 172
    ldrb    r0, [r0, r5]
    ldr     r1, =seal_exp
    ldrb    r1, [r1, r3]
    bl      gf_mul
    eors    r4, r0
    ldr     r2, [r7, #16]
    adds    r3, r3, r2
    cmp     r3, #255
    blo     11f
    subs    r3, #255
11:
    adds    r5, #1
    b       10b
12:
    str     r4, [r7, #24]           @ num
    movs    r4, #0                  @ ---- den = sum_{k odd} C[k] * x^(k-1) ----
    movs    r3, #0
    movs    r5, #1
20:
    ldr     r1, [r7, #0]
    cmp     r5, r1
    bhi     22f
    CTXP    r0, 64
    ldrb    r0, [r0, r5]
    ldr     r1, =seal_exp
    ldrb    r1, [r1, r3]
    bl      gf_mul
    eors    r4, r0
    ldr     r2, [r7, #16]
    lsls    r2, r2, #1
    cmp     r2, #255
    blo     23f
    subs    r2, #255
23:
    adds    r3, r3, r2
    cmp     r3, #255
    blo     21f
    subs    r3, #255
21:
    adds    r5, #2
    b       20b
22:
    cmp     r4, #0
    beq     .Lf_fail
    ldr     r0, [r7, #24]
    mov     r1, r4
    bl      gf_div                  @ num / den
    mov     r4, r0
    ldr     r1, [r7, #12]
    ldr     r2, =seal_exp
    ldrb    r0, [r2, r1]            @ alpha^e
    mov     r1, r4
    bl      gf_mul                  @ y = alpha^e * num / den
    ldr     r1, [r7, #12]
    movs    r2, #151
    subs    r2, r2, r1
    ldr     r3, [r7, #8]
    ldrb    r1, [r3, r2]
    eors    r1, r0
    strb    r1, [r3, r2]
    adds    r6, #1
    b       .Lf_root
.Lf_done:
    movs    r0, #0
    pop     {r4-r7, pc}
.Lf_fail:
    movs    r0, #0
    mvns    r0, r0
    pop     {r4-r7, pc}
    .ltorg

@ ----------------------------------------------------------------------------
@ seal_decode(cw r0) -> r0 = corrected byte count (0..16) or -1
@ ----------------------------------------------------------------------------
    .global seal_decode
    .thumb_func
seal_decode:
    push    {r4-r7, lr}
    sub     sp, #240
    str     r0, [sp, #8]
    mov     r4, r0                  @ cw
    add     r1, sp, #32
    bl      seal_syndromes
    cmp     r0, #0
    beq     .Ld_ok0
    mov     r0, sp
    bl      seal_bm
    cmp     r0, #16
    bhi     .Ld_fail
    mov     r0, sp
    bl      seal_chien
    ldr     r1, [sp, #0]
    cmp     r0, r1
    bne     .Ld_fail
    mov     r0, sp
    bl      seal_forney
    cmp     r0, #0
    bne     .Ld_fail
    mov     r0, r4                  @ re-check: all syndromes must now be zero
    add     r1, sp, #32
    bl      seal_syndromes
    cmp     r0, #0
    bne     .Ld_fail
    ldr     r0, [sp, #0]
    b       .Ld_ret
.Ld_ok0:
    movs    r0, #0
    b       .Ld_ret
.Ld_fail:
    movs    r0, #0
    mvns    r0, r0
.Ld_ret:
    add     sp, #240
    pop     {r4-r7, pc}
    .ltorg

#ifndef AUTH_ENABLE
#define AUTH_ENABLE 1
#endif
#if AUTH_ENABLE
@ ----------------------------------------------------------------------------
@ seal_tag -- 64-bit keyed digest (SipHash-2-4) over a byte run, hand-scheduled for
@ the M0+. State lives in memory as 4 x (lo,hi) words; 32-bit rotates are free
@ (swap the halves).
@
@   void seal_tag(const uint8_t key[16], const uint8_t *data, uint32_t len, uint8_t out[8]);
@        key and data must be 4-byte aligned. out receives the 8 tag bytes (LE).
@ ----------------------------------------------------------------------------
    .thumb_func
sip_round:                          @ r0 = state; clobbers r1-r6
    ldr     r1, [r0, #0]
    ldr     r2, [r0, #4]
    ldr     r3, [r0, #8]
    ldr     r4, [r0, #12]
    adds    r1, r1, r3              @ v0 += v1
    adcs    r2, r4
    lsls    r5, r3, #13             @ v1 = rotl(v1, 13)
    lsrs    r6, r4, #19
    orrs    r5, r6
    lsls    r6, r4, #13
    lsrs    r3, r3, #19
    orrs    r6, r3
    eors    r5, r1                  @ v1 ^= v0
    eors    r6, r2
    str     r2, [r0, #0]            @ v0 = rotl(v0, 32)
    str     r1, [r0, #4]
    str     r5, [r0, #8]
    str     r6, [r0, #12]
    ldr     r1, [r0, #16]
    ldr     r2, [r0, #20]
    ldr     r3, [r0, #24]
    ldr     r4, [r0, #28]
    adds    r1, r1, r3              @ v2 += v3
    adcs    r2, r4
    lsls    r5, r3, #16             @ v3 = rotl(v3, 16)
    lsrs    r6, r4, #16
    orrs    r5, r6
    lsls    r6, r4, #16
    lsrs    r3, r3, #16
    orrs    r6, r3
    eors    r5, r1                  @ v3 ^= v2
    eors    r6, r2
    str     r1, [r0, #16]
    str     r2, [r0, #20]
    ldr     r1, [r0, #0]
    ldr     r2, [r0, #4]
    adds    r1, r1, r5              @ v0 += v3
    adcs    r2, r6
    lsls    r3, r5, #21             @ v3 = rotl(v3, 21)
    lsrs    r4, r6, #11
    orrs    r3, r4
    lsls    r4, r6, #21
    lsrs    r5, r5, #11
    orrs    r4, r5
    eors    r3, r1                  @ v3 ^= v0
    eors    r4, r2
    str     r1, [r0, #0]
    str     r2, [r0, #4]
    str     r3, [r0, #24]
    str     r4, [r0, #28]
    ldr     r1, [r0, #16]
    ldr     r2, [r0, #20]
    ldr     r3, [r0, #8]
    ldr     r4, [r0, #12]
    adds    r1, r1, r3              @ v2 += v1
    adcs    r2, r4
    lsls    r5, r3, #17             @ v1 = rotl(v1, 17)
    lsrs    r6, r4, #15
    orrs    r5, r6
    lsls    r6, r4, #17
    lsrs    r3, r3, #15
    orrs    r6, r3
    eors    r5, r1                  @ v1 ^= v2
    eors    r6, r2
    str     r2, [r0, #16]           @ v2 = rotl(v2, 32)
    str     r1, [r0, #20]
    str     r5, [r0, #8]
    str     r6, [r0, #12]
    bx      lr

    .global seal_tag
    .thumb_func
seal_tag:
    push    {r4-r7, lr}
    sub     sp, #56                 @ [0..31] state, [32] out, [36] len, [40..47] m/tail, [48] blocks
    str     r3, [sp, #32]
    str     r2, [sp, #36]
    mov     r7, r1                  @ data
    ldr     r3, [r0, #0]            @ k0 lo,hi / k1 lo,hi
    ldr     r4, [r0, #4]
    ldr     r5, [r0, #8]
    ldr     r6, [r0, #12]
    ldr     r0, =0x70736575         @ v0 = k0 ^ 0x736f6d6570736575
    eors    r0, r3
    str     r0, [sp, #0]
    ldr     r0, =0x736f6d65
    eors    r0, r4
    str     r0, [sp, #4]
    ldr     r0, =0x6e646f6d         @ v1 = k1 ^ 0x646f72616e646f6d
    eors    r0, r5
    str     r0, [sp, #8]
    ldr     r0, =0x646f7261
    eors    r0, r6
    str     r0, [sp, #12]
    ldr     r0, =0x6e657261         @ v2 = k0 ^ 0x6c7967656e657261
    eors    r0, r3
    str     r0, [sp, #16]
    ldr     r0, =0x6c796765
    eors    r0, r4
    str     r0, [sp, #20]
    ldr     r0, =0x79746573         @ v3 = k1 ^ 0x7465646279746573
    eors    r0, r5
    str     r0, [sp, #24]
    ldr     r0, =0x74656462
    eors    r0, r6
    str     r0, [sp, #28]
    ldr     r4, [sp, #36]
    lsrs    r4, r4, #3              @ whole 8-byte blocks
    beq     2f
    str     r4, [sp, #48]
1:
    ldmia   r7!, {r2, r3}           @ m
    str     r2, [sp, #40]
    str     r3, [sp, #44]
    ldr     r0, [sp, #24]
    eors    r0, r2
    str     r0, [sp, #24]           @ v3 ^= m
    ldr     r0, [sp, #28]
    eors    r0, r3
    str     r0, [sp, #28]
    mov     r0, sp
    bl      sip_round
    mov     r0, sp
    bl      sip_round
    ldr     r2, [sp, #40]
    ldr     r3, [sp, #44]
    ldr     r0, [sp, #0]
    eors    r0, r2
    str     r0, [sp, #0]            @ v0 ^= m
    ldr     r0, [sp, #4]
    eors    r0, r3
    str     r0, [sp, #4]
    ldr     r4, [sp, #48]
    subs    r4, #1
    str     r4, [sp, #48]
    bne     1b
2:                                  @ tail: remaining len & 7 bytes + length byte
    movs    r0, #0
    str     r0, [sp, #40]
    str     r0, [sp, #44]
    ldr     r2, [sp, #36]
    movs    r3, #7
    ands    r3, r2                  @ r3 = bytes left
    beq     4f
    add     r1, sp, #40
3:
    ldrb    r0, [r7]
    adds    r7, #1
    strb    r0, [r1]
    adds    r1, #1
    subs    r3, #1
    bne     3b
4:
    ldr     r5, [sp, #40]           @ b lo
    ldr     r6, [sp, #44]           @ b hi
    ldr     r2, [sp, #36]
    lsls    r2, r2, #24
    orrs    r6, r2                  @ length in the top byte
    str     r5, [sp, #40]
    str     r6, [sp, #44]
    ldr     r0, [sp, #24]
    eors    r0, r5
    str     r0, [sp, #24]           @ v3 ^= b
    ldr     r0, [sp, #28]
    eors    r0, r6
    str     r0, [sp, #28]
    mov     r0, sp
    bl      sip_round
    mov     r0, sp
    bl      sip_round
    ldr     r5, [sp, #40]
    ldr     r6, [sp, #44]
    ldr     r0, [sp, #0]
    eors    r0, r5
    str     r0, [sp, #0]            @ v0 ^= b
    ldr     r0, [sp, #4]
    eors    r0, r6
    str     r0, [sp, #4]
    ldr     r0, [sp, #16]
    movs    r1, #255
    eors    r0, r1
    str     r0, [sp, #16]           @ v2 ^= 0xff
    mov     r0, sp
    bl      sip_round
    mov     r0, sp
    bl      sip_round
    mov     r0, sp
    bl      sip_round
    mov     r0, sp
    bl      sip_round
    ldr     r0, [sp, #0]            @ out = v0 ^ v1 ^ v2 ^ v3
    ldr     r1, [sp, #8]
    eors    r0, r1
    ldr     r1, [sp, #16]
    eors    r0, r1
    ldr     r1, [sp, #24]
    eors    r0, r1
    ldr     r2, [sp, #4]
    ldr     r1, [sp, #12]
    eors    r2, r1
    ldr     r1, [sp, #20]
    eors    r2, r1
    ldr     r1, [sp, #28]
    eors    r2, r1
    ldr     r3, [sp, #32]
    str     r0, [r3, #0]
    str     r2, [r3, #4]
    add     sp, #56
    pop     {r4-r7, pc}
    .ltorg
#endif /* AUTH_ENABLE */
