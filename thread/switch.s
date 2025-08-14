[bits 32]
section .text
; switch_to(cur,next)
global switch_to
switch_to:
	; this place is ret_addr
	push esi
	push edi
	push ebx
	push ebp

	mov eax,[esp+20] ; get $[cur]
	mov [eax],esp

	mov eax,[esp+24] ; get $[next]
	mov esp,[eax]

	pop ebp
	pop ebx
	pop edi
	pop esi
	ret