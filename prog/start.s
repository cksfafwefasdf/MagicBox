[bits 32]
extern main
extern exit
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