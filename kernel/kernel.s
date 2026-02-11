[bits 32]
%define ERROR_CODE nop
%define ZERO push 0

extern put_str
extern idt_ISR

section .data:
global intr_entry_table
intr_entry_table:

%macro VECTOR 2
section .text
intr%1entry:
	
	%2
	
	push ds
	push es
	push fs
	push gs
	pushad

	; IRQ0~IRQ15 is caused by external device, we need send EOI to 8259A
	; 0x0~0x1f is not caused by external decvice,so we dont need to send EOI to 8259A
	%if %1>=0x20 && %1<=0x2f 
		mov al,0x20 ;command EOI, 001_00000b normal-EOI,8259A will set ISR zero correspondly after EOI
		out 0x20,al ;send to OCW2-master
		%if %1>=0x28
			out 0xa0,al ;send to OCW2-slave
		%endif
	%endif

	push %1 ;push intr-vector
	call [idt_ISR+4*%1]
	
	jmp intr_exit


section .data:
	dd intr%1entry
%endmacro

section .text
global intr_exit
intr_exit:
    ; add esp, 4 ; 跳过 vec_no。此时 esp 指向 edi

    ; 检查 CS 确定是否返回用户态 
    ; 偏移量 = edi(0) + 8*4(通用) + 4*4(段) + 2*4(err, eip) = 56 
	; 也就是之前pushad保存的环境
	; popad 和 pop 段寄存器的操作要放到该操作后，否则可能会返回错误的返回值
    mov eax, [esp + 60]     
	; 检查是否是返回到用户态进程中，检查CS寄存器的的 RPL 位
	; 看看待返回的代码是否是用户代码
	; 该段代码就是内核态代码，因此检查待转目标是否是用户态就行
    and eax, 0x0003
    cmp eax, 0x0003
    jne .restore_context    ; 内核态返回，直接走

    ; 只有在内核态返回用户态时，才调用信号处理
    push esp ; 此时 esp 指向 edi，正是 struct intr_stack*
    extern do_signal
    call do_signal
    add esp, 4

.restore_context:
	add esp, 4 ; 跳过 vec_no。此时 esp 指向 edi
    ; 严格按照结构体顺序恢复
    
    popad ; 恢复 edi 到 eax (8个)
    pop gs ; 恢复段寄存器 (4个)
    pop fs
    pop es
    pop ds
    
    add esp, 4 ; 跳过 err_code
    iretd

extern syscall_table
section .text
global syscall_handler
syscall_handler:
	; formalize the stack, just like ZERO, in order to adapt the structure of intr_stack
	; push 0 equals to push error_code
	push 0 
	push ds
	push es
	push fs
	push gs
	pushad

	push 0x80 ; just formalize the stack
	push edi ; arg5
	push esi ; arg4
	push edx ; arg3
	push ecx ; arg2
	push ebx ; arg1
	
	call [syscall_table+eax*4]
	add esp,20 ; skip arg1,2,3,4,5 recycle stack

	mov [esp+4*8],eax ; get the return value
	jmp intr_exit ; restore context


global syscall_handler_convey_by_stack
syscall_handler_convey_by_stack:
	push 0 
	push ds
	push es
	push fs
	push gs
	pushad

	push 0x80 ; just formalize the stack

	; get user stack esp
	; 48 = 4*(8+4)
	; means 8 regs in pushad = 4*8 = 32
	; 4 seg-regs = 4*2(seg-reg is 16bits) = 16
	; so 32+16=48 in total
	mov ebx,[esp+4+48+4+12]
	
	push dword [ebx+12] ; arg3
	push dword [ebx+8] ; arg2
	push dword [ebx+4] ; arg1
	mov edx,[ebx] ; syscall_num

	call [syscall_table+edx*4]
	add esp,12 ; skip arg1,2,3, recycle stack

	mov [esp+4*8],eax
	jmp intr_exit ; restore context

VECTOR 0x00,ZERO
VECTOR 0X01,ZERO
VECTOR 0X02,ZERO
VECTOR 0x03,ZERO
VECTOR 0X04,ZERO
VECTOR 0X05,ZERO
VECTOR 0x06,ZERO
VECTOR 0X07,ZERO
VECTOR 0X08,ERROR_CODE
VECTOR 0x09,ZERO
VECTOR 0X0a,ERROR_CODE
VECTOR 0X0b,ERROR_CODE
VECTOR 0x0c,ERROR_CODE
VECTOR 0X0d,ERROR_CODE
VECTOR 0X0e,ERROR_CODE
VECTOR 0x0f,ZERO
VECTOR 0X10,ZERO
VECTOR 0X11,ERROR_CODE
VECTOR 0x12,ZERO
VECTOR 0X13,ZERO
VECTOR 0X14,ZERO
VECTOR 0x15,ZERO
VECTOR 0X16,ZERO
VECTOR 0X17,ZERO
VECTOR 0x18,ZERO
VECTOR 0X19,ZERO
VECTOR 0X1a,ZERO
VECTOR 0x1b,ZERO
VECTOR 0X1c,ZERO
VECTOR 0X1d,ZERO
VECTOR 0x1e,ZERO
VECTOR 0x1f,ZERO

VECTOR 0x20,ZERO ; timer intr
VECTOR 0x21,ZERO ; keyboard intr
VECTOR 0x22,ZERO ; for cascading 
VECTOR 0x23,ZERO ; entry for serial port 2
VECTOR 0x24,ZERO ; entry for serial port 1
VECTOR 0x25,ZERO ; entry for parallel port 2
VECTOR 0x26,ZERO ; entry for floppy
VECTOR 0x27,ZERO ; entry for parallel port 1
VECTOR 0x28,ZERO ; entry for real-time clock
VECTOR 0x29,ZERO ; redirection
VECTOR 0x2a,ZERO ; reserved
VECTOR 0x2b,ZERO ; reserved
VECTOR 0x2c,ZERO ; ps/2 mouse
VECTOR 0x2d,ZERO ; fpu (float processor) fault
VECTOR 0x2e,ZERO ; hard disk
VECTOR 0x2f,ZERO ; reserved

