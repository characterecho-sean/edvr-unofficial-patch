option casemap:none

EXTERN sourceOwnerTestCapture:PROC
EXTERN recordWriterUnwindCapture:PROC
PUBLIC sourceOwnerUnwindStub
PUBLIC recordWriterUnwindStub

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
    sub rsp,1B8h
    .allocstack 1B8h
    .endprolog
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
    ret
recordWriterUnwindStub ENDP
_TEXT ENDS
END
