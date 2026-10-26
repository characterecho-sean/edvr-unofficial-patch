EXTERN g_weaponUiGetterTarget:QWORD
EXTERN g_weaponUiProbeCalls:QWORD
EXTERN g_weaponUiProbeArgs:QWORD
EXTERN weaponUiProbeRecordObservation:PROC
EXTERN weaponUiGetterCallStub:PROC
PUBLIC weaponUiProbeFakeGetter
PUBLIC weaponUiProbeTestInvoke

.data
ALIGN 16
probeMxcsr DWORD 03F80h
probeCallbackMxcsr DWORD 01F80h
ALIGN 16
probeXmm0 REAL4 1.0, 2.0, 3.0, 4.0
probeXmm1 REAL4 5.0, 6.0, 7.0, 8.0
probeXmm2 REAL4 9.0, 10.0, 11.0, 12.0
probeXmm3 REAL4 13.0, 14.0, 15.0, 16.0
probeXmm4 REAL4 17.0, 18.0, 19.0, 20.0
probeXmm5 REAL4 21.0, 22.0, 23.0, 24.0
probeMatrix REAL4 101.0, 102.0, 103.0, 104.0
            REAL4 105.0, 106.0, 107.0, 108.0
            REAL4 109.0, 110.0, 111.0, 112.0
            REAL4 113.0, 114.0, 115.0, 116.0

.code
weaponUiProbeFakeGetter PROC
    inc qword ptr [g_weaponUiProbeCalls]
    mov qword ptr [g_weaponUiProbeArgs], rcx
    mov qword ptr [g_weaponUiProbeArgs+8], rdx
    mov r10, rdx
    lea r11, probeMatrix
    mov rax, qword ptr [r11+00h]
    mov qword ptr [r10+00h], rax
    mov rax, qword ptr [r11+08h]
    mov qword ptr [r10+08h], rax
    mov rax, qword ptr [r11+10h]
    mov qword ptr [r10+10h], rax
    mov rax, qword ptr [r11+18h]
    mov qword ptr [r10+18h], rax
    mov rax, qword ptr [r11+20h]
    mov qword ptr [r10+20h], rax
    mov rax, qword ptr [r11+28h]
    mov qword ptr [r10+28h], rax
    mov rax, qword ptr [r11+30h]
    mov qword ptr [r10+30h], rax
    mov rax, qword ptr [r11+38h]
    mov qword ptr [r10+38h], rax
    movdqu xmm0, xmmword ptr [probeXmm0]
    movdqu xmm1, xmmword ptr [probeXmm1]
    movdqu xmm2, xmmword ptr [probeXmm2]
    movdqu xmm3, xmmword ptr [probeXmm3]
    movdqu xmm4, xmmword ptr [probeXmm4]
    movdqu xmm5, xmmword ptr [probeXmm5]
    ldmxcsr dword ptr [probeMxcsr]
    mov rax, rdx
    mov rcx, 0C1h
    mov rdx, 0D2h
    mov r8, 0E8h
    mov r9, 0E9h
    mov r10, 0EAh
    mov r11, 0EBh
    cmp rax, rax
    ret
weaponUiProbeFakeGetter ENDP

weaponUiGetterObserved PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    call weaponUiProbeRecordObservation
    pxor xmm0, xmm0
    pxor xmm1, xmm1
    pxor xmm2, xmm2
    pxor xmm3, xmm3
    pxor xmm4, xmm4
    pxor xmm5, xmm5
    ldmxcsr dword ptr [probeCallbackMxcsr]
    mov rax, 0A0h
    mov rcx, 0C2h
    mov rdx, 0D3h
    mov r8, 0E4h
    mov r9, 0E5h
    mov r10, 0E6h
    mov r11, 0E7h
    add rsp, 28h
    stc
    ret
weaponUiGetterObserved ENDP

weaponUiProbeTestInvoke PROC FRAME
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 0B8h
    .allocstack 0B8h
    .endprolog
    mov rbx, rcx
    mov r14, rdx
    mov rdi, r8
    mov r15, r9
    mov rcx, 011h
    lea rdx, [rsp+70h]
    mov r8, 022h
    mov r9, 033h
    call weaponUiGetterCallStub
    mov qword ptr [rbx+00h], rax
    mov qword ptr [rbx+08h], rcx
    mov qword ptr [rbx+10h], rdx
    mov qword ptr [rbx+18h], r8
    mov qword ptr [rbx+20h], r9
    mov qword ptr [rbx+28h], r10
    mov qword ptr [rbx+30h], r11
    pushfq
    pop r10
    mov qword ptr [rbx+38h], r10
    movdqu xmmword ptr [rbx+40h], xmm0
    movdqu xmmword ptr [rbx+50h], xmm1
    movdqu xmmword ptr [rbx+60h], xmm2
    movdqu xmmword ptr [rbx+70h], xmm3
    movdqu xmmword ptr [rbx+80h], xmm4
    movdqu xmmword ptr [rbx+90h], xmm5
    stmxcsr dword ptr [rbx+0A0h]
    add rsp, 0B8h
    pop r15
    pop r14
    pop rdi
    pop rbx
    ret
weaponUiProbeTestInvoke ENDP
END
