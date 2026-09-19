option casemap:none

EXTERN sourceOwnerTestCapture:PROC
EXTERN recordWriterUnwindCapture:PROC
EXTERN recordWriterOwnershipUnwindCapture:PROC
PUBLIC sourceOwnerUnwindStub
PUBLIC recordWriterUnwindStub
PUBLIC kinematicOwnerDirectUnwindStub
PUBLIC kinematicOwnerVirtualUnwindStub

_TEXT SEGMENT
sourceOwnerUnwindStub PROC FRAME
    push rbx
    .pushreg rbx
    sub rsp,20h
    .allocstack 20h
    .endprolog
    mov rbx,rcx
    call sourceOwnerTestCapture
sourceOwnerUnwindResume LABEL NEAR
PUBLIC sourceOwnerUnwindResume
    add rsp,20h
    pop rbx
    ret
sourceOwnerUnwindStub ENDP

; Copy one exact 0x150-byte record into the direct-writer caller's stack
; layout, then call a normal C++ observer.  Its RtlCaptureContext must unwind
; back to recordWriterUnwindResume without first unwinding this frame.
; RCX=dictionary, RDX=key, R8=record source.
recordWriterUnwindStub PROC FRAME
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    sub rsp,1B8h
    .allocstack 1B8h
    .endprolog
    mov rbx,1111111111111111h
    mov rdi,2222222222222222h
    mov [rsp+20h],rcx
    mov [rsp+28h],rdx
    lea r10,[rsp+50h]
    xor r11d,r11d
recordWriterCopy:
    mov rax,[r8+r11*8]
    mov [r10+r11*8],rax
    inc r11d
    cmp r11d,2Ah
    jb recordWriterCopy
    mov rcx,[rsp+20h]
    mov rdx,[rsp+28h]
    call recordWriterUnwindCapture
recordWriterUnwindResume LABEL NEAR
PUBLIC recordWriterUnwindResume
    add rsp,1B8h
    pop rdi
    pop rbx
    ret
recordWriterUnwindStub ENDP

; A writer-shaped frame with the direct_43130aa record at caller RSP+60h.
; It deliberately clobbers RBX/RDI after saving them, so the ownership probe
; only sees the ancestor's values if production RtlVirtualUnwind restores them.
recordWriterOwnershipUnwindStub PROC FRAME
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    sub rsp,1C8h
    .allocstack 1C8h
    .endprolog
    mov rbx,3333333333333333h
    mov rdi,4444444444444444h
    mov [rsp+20h],rcx
    mov [rsp+28h],rdx
    lea r10,[rsp+60h]
    xor r11d,r11d
recordWriterOwnershipCopy:
    mov rax,[r8+r11*8]
    mov [r10+r11*8],rax
    inc r11d
    cmp r11d,2Ah
    jb recordWriterOwnershipCopy
    mov rcx,[rsp+20h]
    mov rdx,[rsp+28h]
    call recordWriterOwnershipUnwindCapture
recordWriterOwnershipUnwindResume LABEL NEAR
PUBLIC recordWriterOwnershipUnwindResume
    add rsp,1C8h
    pop rdi
    pop rbx
    ret
recordWriterOwnershipUnwindStub ENDP

; RCX=outer, RDX=dictionary, R8=key, R9=record source.
kinematicOwnerDirectUnwindStub PROC FRAME
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    sub rsp,38h
    .allocstack 38h
    .endprolog
    mov [rsp+20h],rdx
    mov [rsp+28h],r8
    mov [rsp+30h],r9
    mov rdi,rcx
    mov rbx,[rcx+348h]
    mov rcx,[rsp+20h]
    mov rdx,[rsp+28h]
    mov r8,[rsp+30h]
    call recordWriterOwnershipUnwindStub
kinematicOwnerDirectUnwindResume LABEL NEAR
PUBLIC kinematicOwnerDirectUnwindResume
    add rsp,38h
    pop rdi
    pop rbx
    ret
kinematicOwnerDirectUnwindStub ENDP

kinematicOwnerVirtualUnwindStub PROC FRAME
    push rbx
    .pushreg rbx
    push rdi
    .pushreg rdi
    sub rsp,38h
    .allocstack 38h
    .endprolog
    mov [rsp+20h],rdx
    mov [rsp+28h],r8
    mov [rsp+30h],r9
    mov rdi,rcx
    mov rbx,[rcx+348h]
    mov rcx,[rsp+20h]
    mov rdx,[rsp+28h]
    mov r8,[rsp+30h]
    call recordWriterOwnershipUnwindStub
kinematicOwnerVirtualUnwindResume LABEL NEAR
PUBLIC kinematicOwnerVirtualUnwindResume
    add rsp,38h
    pop rdi
    pop rbx
    ret
kinematicOwnerVirtualUnwindStub ENDP
_TEXT ENDS
END
