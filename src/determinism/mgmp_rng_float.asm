; rand2 keeps XMM2..5 live across the native leaf randfloat call (94F5AD,
; 94F716). Preserve the native output across the C++ recorder, including
; XMM1 and volatile integer registers. The returned random value is XMM0.
EXTERN mgmp_rng_native_randfloat:QWORD
EXTERN mgmp_rng_observe_float:PROC
.code
mgmp_rng_randfloat PROC FRAME
    sub rsp, 108h
    .allocstack 108h
    .endprolog
    mov [rsp+0E0h], rax
    mov [rsp+20h], rdx
    mov rax, [rdx]
    mov [rsp+28h], rax
    mov rax, [rsp+108h]
    mov [rsp+30h], rax
    mov rax, [rsp+0E0h]
    call QWORD PTR [mgmp_rng_native_randfloat]
    mov [rsp+40h], rax
    mov [rsp+48h], rcx
    mov [rsp+50h], rdx
    mov [rsp+58h], r8
    mov [rsp+60h], r9
    mov [rsp+68h], r10
    mov [rsp+70h], r11
    movaps [rsp+80h], xmm0
    movaps [rsp+90h], xmm1
    movaps [rsp+0A0h], xmm2
    movaps [rsp+0B0h], xmm3
    movaps [rsp+0C0h], xmm4
    movaps [rsp+0D0h], xmm5
    mov rcx, [rsp+30h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+80h]
    mov r9, [rsp+20h]
    call mgmp_rng_observe_float
    movaps xmm0, [rsp+80h]
    movaps xmm1, [rsp+90h]
    movaps xmm2, [rsp+0A0h]
    movaps xmm3, [rsp+0B0h]
    movaps xmm4, [rsp+0C0h]
    movaps xmm5, [rsp+0D0h]
    mov rax, [rsp+40h]
    mov rcx, [rsp+48h]
    mov rdx, [rsp+50h]
    mov r8, [rsp+58h]
    mov r9, [rsp+60h]
    mov r10, [rsp+68h]
    mov r11, [rsp+70h]
    add rsp, 108h
    ret
mgmp_rng_randfloat ENDP
END
