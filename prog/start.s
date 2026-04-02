[bits 32]
extern main
extern exit
extern SYS_SIGRETURN

section .text
; global _start
; _start:
; 	; this two regs must correspond with the regs in function execv
; 	push ebx ; push argv
; 	push ecx ; push argc
; 	; int main(argc,argv)
; 	call main

; 	push eax ; restore the return value of main
; 	call exit ; exit will not go back
global _start
_start:
    ; 此时栈顶布局 (ESP 指向的地方):
    ; [esp]     -> argc
    ; [esp + 4] -> argv[0] 指针
    ; [esp + 8] -> argv[1] 指针
    ; ...

    ; 获取 argc
    mov eax, [esp]      
    
    ; 获取 argv 的地址 (即 argc 旁边的位置)
    lea ebx, [esp + 4]  

    ; 按照 C 调用约定压栈传参给 main(int argc, char** argv)
    push ebx            ; 压入 argv 指针
    push eax            ; 压入 argc 整数
    
    call main

    ; 退出处理
    push eax            ; main 的返回值
    call exit


%define SYS_SIGRETURN 40

global sig_restorer
; 为了防止出现 栈帧切换或者汇编优化等情况破坏我们在 setup_frame 函数中布置好的栈
; 我们sig_restorer最好用汇编来写
sig_restorer:
	
    mov eax, SYS_SIGRETURN
	; 我们自己系统的中断号改为了0x77
    int 0x77
    ; 理论上永远不会执行到这里，因为内核已经执行 iret 走了
	hlt