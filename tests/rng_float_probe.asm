.const
ALIGN 16
sentinel QWORD 3FE0000000000000h, 0123456789ABCDEFh
.code
; Same optimized-call pattern as game rand2: XMM2..5 stay live across randfloat.
rng_float_probe PROC FRAME
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    mov rbx, rcx
    mov rsi, r8
    movaps xmm0, xmm3
    movaps xmm1, XMMWORD PTR [sentinel]
    movaps xmm2, XMMWORD PTR [sentinel]
    movaps xmm3, XMMWORD PTR [sentinel]
    movaps xmm4, XMMWORD PTR [sentinel]
    movaps xmm5, XMMWORD PTR [sentinel]
    call rbx
    movups [rsi], xmm0
    movups [rsi+10h], xmm1
    movups [rsi+20h], xmm2
    movups [rsi+30h], xmm3
    movups [rsi+40h], xmm4
    movups [rsi+50h], xmm5
    add rsp, 28h
    pop rsi
    pop rbx
    ret
rng_float_probe ENDP

; A legal C++ callee can freely destroy all volatile SIMD registers.
rng_float_clobber PROC
    pxor xmm0, xmm0
    pxor xmm1, xmm1
    pxor xmm2, xmm2
    pxor xmm3, xmm3
    pxor xmm4, xmm4
    pxor xmm5, xmm5
    ret
rng_float_clobber ENDP
END
