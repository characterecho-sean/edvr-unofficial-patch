option casemap:none

EXTERN sourceOwnerTestCapture:PROC
PUBLIC sourceOwnerUnwindStub

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
_TEXT ENDS
END
