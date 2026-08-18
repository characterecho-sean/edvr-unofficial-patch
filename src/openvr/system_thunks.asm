; IVRSystem_012 micro-thunks for the two struct-returning slots.
;
; GetProjectionMatrix (slot 1) and GetEyeToHeadTransform (slot 4) return
; 64- and 48-byte structs by value. A compiled experiment (EVIDENCE 6bo,
; 2026-08-18) showed MSVC's member and free-function conventions disagree
; about where the hidden return pointer rides for such methods: member is
; (this, retslot, args...), free is (retslot, args...). A C receiver of
; either shape therefore corrupts a caller of the other -- and not loudly:
; the return VALUE still arrives (callers read it back through RAX), while
; the struct is written over whatever the first pointer names. The object
; dies; the crash comes on a later dispatch, far from the cause. That is
; how the archived probe_openvr read correct matrices out of vrclient in
; 2026-08-07 (EVIDENCE 4.2/4.3) while silently clobbering vrclient's own
; IVRSystem object.
;
; So these two slots are observed by thunks that touch NO argument register
; and tail-jump to the original: correct under every calling convention,
; because the thunk never has an opinion about which one is in use. They
; count the call, and the first call through each spills the argument
; registers for the C side to decode offline -- which register holds the
; interface pointer TELLS us the caller's convention, from the field.
;
; No PROC frame, no unwind info, on purpose: nothing here moves rsp, so at
; the jmp target the stack is exactly as the caller made it and an unwinder
; walks straight past us using the caller's own .pdata. (The generated lazy
; export shim needs unwind info because it moves rsp; these must not, and
; build.bat's .ENDPROLOG assertion deliberately does not apply here.)
;
; RAX is used as scratch for the two stack arguments: at a non-vararg call
; boundary RAX carries nothing, and every convention in play agrees it is
; caller-saved.
;
; All data lives in system_hook.cpp (extern "C"); this file only references
; it. Layout contracts, asserted on the C side with static_assert:
;   edvr_sysCounts    dword[8], indexed by slot
;   edvr_sysCapClaim  dword[8], bit 0 = a thread won the capture
;   edvr_sysCapDone   dword[8], 1 = capture words are all written
;   edvr_sysCapture   qword[8][7]: rcx rdx r8 r9 xmm3 [rsp+28h] [rsp+30h]
;   edvr_sysOrig      qword[8], the forward targets, written before commit

EXTERN edvr_sysCounts:DWORD
EXTERN edvr_sysCapClaim:DWORD
EXTERN edvr_sysCapDone:DWORD
EXTERN edvr_sysCapture:QWORD
EXTERN edvr_sysOrig:QWORD

.code

; slot 1: GetProjectionMatrix. Capture area = edvr_sysCapture + 1*56.
edvr_sysThunkMatrix PROC
    lock inc dword ptr [edvr_sysCounts+4]
    lock bts dword ptr [edvr_sysCapClaim+4], 0
    jc   fwd_matrix
    mov  qword ptr [edvr_sysCapture+56],  rcx
    mov  qword ptr [edvr_sysCapture+64],  rdx
    mov  qword ptr [edvr_sysCapture+72],  r8
    mov  qword ptr [edvr_sysCapture+80],  r9
    movq qword ptr [edvr_sysCapture+88],  xmm3
    mov  rax, qword ptr [rsp+28h]
    mov  qword ptr [edvr_sysCapture+96],  rax
    mov  rax, qword ptr [rsp+30h]
    mov  qword ptr [edvr_sysCapture+104], rax
    mov  dword ptr [edvr_sysCapDone+4], 1
fwd_matrix:
    jmp  qword ptr [edvr_sysOrig+8]
edvr_sysThunkMatrix ENDP

; slot 4: GetEyeToHeadTransform. Capture area = edvr_sysCapture + 4*56.
edvr_sysThunkEyeToHead PROC
    lock inc dword ptr [edvr_sysCounts+16]
    lock bts dword ptr [edvr_sysCapClaim+16], 0
    jc   fwd_eye
    mov  qword ptr [edvr_sysCapture+224], rcx
    mov  qword ptr [edvr_sysCapture+232], rdx
    mov  qword ptr [edvr_sysCapture+240], r8
    mov  qword ptr [edvr_sysCapture+248], r9
    movq qword ptr [edvr_sysCapture+256], xmm3
    mov  rax, qword ptr [rsp+28h]
    mov  qword ptr [edvr_sysCapture+264], rax
    mov  rax, qword ptr [rsp+30h]
    mov  qword ptr [edvr_sysCapture+272], rax
    mov  dword ptr [edvr_sysCapDone+16], 1
fwd_eye:
    jmp  qword ptr [edvr_sysOrig+32]
edvr_sysThunkEyeToHead ENDP

END
