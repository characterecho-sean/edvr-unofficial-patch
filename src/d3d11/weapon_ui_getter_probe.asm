EXTERN g_weaponUiGetterTarget:QWORD
EXTERN weaponUiGetterObserved:PROC
PUBLIC weaponUiGetterCallStub

.code
weaponUiGetterCallStub PROC FRAME
    sub rsp, 4F8h
    .allocstack 4F8h
    .endprolog

    ; The updater's renderer, part and camera registers survive the getter by the x64 ABI.
    mov qword ptr [rsp+80h], r14
    mov qword ptr [rsp+88h], rdi
    mov qword ptr [rsp+90h], r15

    call qword ptr [g_weaponUiGetterTarget]

    ; RDX is the original caller's output buffer (updaterRSP+70h, stubEntryRSP+78h); the getter returns that pointer in RAX.
    ; Save every volatile return register, flags and the legacy FP/SSE state around the diagnostic callback.
    mov qword ptr [rsp+0A0h], rax
    mov qword ptr [rsp+0A8h], rcx
    mov qword ptr [rsp+0B0h], rdx
    mov qword ptr [rsp+0B8h], r8
    mov qword ptr [rsp+0C0h], r9
    mov qword ptr [rsp+0C8h], r10
    mov qword ptr [rsp+0D0h], r11
    pushfq
    pop rax
    mov qword ptr [rsp+0D8h], rax
    fxsave64 [rsp+100h]

    lea r10, [rsp+570h]
    lea r11, [rsp+318h]
    mov rax, qword ptr [r10+00h]
    mov qword ptr [r11+00h], rax
    mov rax, qword ptr [r10+08h]
    mov qword ptr [r11+08h], rax
    mov rax, qword ptr [r10+10h]
    mov qword ptr [r11+10h], rax
    mov rax, qword ptr [r10+18h]
    mov qword ptr [r11+18h], rax
    mov rax, qword ptr [r10+20h]
    mov qword ptr [r11+20h], rax
    mov rax, qword ptr [r10+28h]
    mov qword ptr [r11+28h], rax
    mov rax, qword ptr [r10+30h]
    mov qword ptr [r11+30h], rax
    mov rax, qword ptr [r10+38h]
    mov qword ptr [r11+38h], rax
    mov rax, qword ptr [rsp+80h]
    mov qword ptr [rsp+300h], rax
    mov rax, qword ptr [rsp+88h]
    mov qword ptr [rsp+308h], rax
    mov rax, qword ptr [rsp+90h]
    mov qword ptr [rsp+310h], rax
    lea rcx, [rsp+300h]
    call weaponUiGetterObserved

    fxrstor64 [rsp+100h]
    push qword ptr [rsp+0D8h]
    popfq
    mov rcx, qword ptr [rsp+0A8h]
    mov rdx, qword ptr [rsp+0B0h]
    mov r8,  qword ptr [rsp+0B8h]
    mov r9,  qword ptr [rsp+0C0h]
    mov r10, qword ptr [rsp+0C8h]
    mov r11, qword ptr [rsp+0D0h]
    mov rax, qword ptr [rsp+0A0h]
    lea rsp, [rsp+4F8h]
    ret
weaponUiGetterCallStub ENDP
END
