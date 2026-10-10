@ ============================================================================
@ seal_tag.S -- hand-written Thumb-1 (Cortex-M0+) keyed 64-bit digest used by the packet integrity check
@ (Mission/seal_wrap.c). Assembled only when AUTH_ENABLE=1. Tested under Unicorn against a Python model and the
@ official test vector by tests/tag_asm/test_seal_tag.py.
@ All routines follow the AAPCS (r4-r7 preserved, r0-r3/r12 scratch).
@ ============================================================================
    .syntax unified
    .cpu cortex-m0plus
    .thumb

#ifndef AUTH_ENABLE
#define AUTH_ENABLE 1
#endif
#if AUTH_ENABLE
    .section .text.seal_tag,"ax",%progbits
    .align 2

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
