[bits 32]

extern swap_page
extern write_protect

section .text
; void intr_handler_page_fault(void);
global intr_handler_page_fault
intr_handler_page_fault:
	xor eax,eax
	mov eax,esp
	; The pushad instruction pushes 8 elements. 
	; Adding the pushed segment registers (ds, es, fs, gs) and the interrupt vector makes a total of 13 elements. 
	; The error code is the 14th element.
	add eax,14*4 
	mov eax,[eax] ; get err_code 
	mov ebx,cr2 ; get the virtual address that caused the page fault.
	; push the arguments for the function call
	
	; void swap_page(uint32_t err_code,void* err_vaddr);
	; void write_protect(uint32_t err_code,void* err_vaddr);
	push ebx
	push eax

	test eax,1 ; test flag P
	jz .swap_page
	; if P is 1, this faulte is caused by write protection
	call write_protect
	jmp .done

.swap_page:
	call swap_page
.done: 
	add esp,8 ; clean the stack
	ret