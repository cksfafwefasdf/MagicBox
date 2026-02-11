[bits 32]
extern main
extern exit
extern SYS_SIGRETURN

section .text
global _start
_start:
	; this two regs must correspond with the regs in function execv
	push ebx ; push argv
	push ecx ; push argc
	; int main(argc,argv)
	call main

	push eax ; restore the return value of main
	call exit ; exit will not go back


%define SYS_SIGRETURN 40

global sig_restorer
; 为了防止出现 栈帧切换或者汇编优化等情况破坏我们在 setup_frame 函数中布置好的栈
; 我们sig_restorer最好用汇编来写
sig_restorer:
	
    mov eax, SYS_SIGRETURN
    int 0x80
    ; 理论上永远不会执行到这里，因为内核已经执行 iret 走了
	hlt