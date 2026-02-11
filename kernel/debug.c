#include "debug.h"
#include "print.h"
#include "interrupt.h"
#include "stdio.h"
#include "syscall.h"

void panic_spin(char* filename,int line,const char* func,const char* condition){
   	intr_disable();
    put_str("\n\n\n!!!!!!!!!! error !!!!!!!!!!\n");
    put_str("filename: ");put_str(filename);put_str("\n");
    put_str("line: 0x");put_int(line);put_str("\n");
    put_str("function: ");put_str((char*)func);put_str("\n");
    put_str("condition: ");put_str((char*)condition);put_str("\n");

    // 开始栈回溯
    // C 语言在编译成汇编代码时，遵循了一套标准化的函数调用规约
    // 在进入新函数前，会先把当前栈底指针ebp进行压栈，再执行 mov ebp,esp 来切换栈帧
    // 这意味着当前的栈底指针ebp指向的其实是上一层函数调用的栈底，这些栈底指针就形成了一个链表 
    // 如果加了 -fomit-frame-pointer 编译优化的话，这招就不灵了，因为编译器会把这种栈帧的切换方式给优化掉
    // 还有需要注意的一点是我们需要手动在内核最开始启动的地方（比如 loader 跳转到 main 之前）把 ebp 清零。
    // 这样回溯循环判断 while(ebp != 0) 才能正常停下来，否则它会一直往低地址读，直到触发 GP 错误

    
    print_stacktrace();
    
    
    put_str("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    while(1);
}

void print_stacktrace(){
    uint32_t* ebp;
    asm volatile ("movl %%ebp, %0" : "=r" (ebp));

    put_str("stack backtrace:");
    // 由于内核栈在 PCB 页面的顶端，栈底通常是对齐到页面边界的。
    // 只要 ebp 指针还在当前内核栈的有效范围内（1页之内），我们就继续。
    
    while (ebp != NULL) {
        uint32_t ret_addr = *(ebp + 1); // ebp + 4 字节处是返回地址
        if (ret_addr == 0) break; // 摸到头了（通常内核起始入口会清零 ebp）
        
        // 需要注意的一点是，ret_addr并不是某个函数的入口地址，而是发生函数调用时的地址
        // 比如 A 在第7行调用了B，那么这个ret_addr的地址是A第7行的地址，而不是A的起始入口地址
        // 这一点需要注意，因此我们拿到ret_addr在kernel.map中找入口时，应当要往前找
        put_str(" [0x"); put_int(ret_addr); put_str("]");
        
        // 取出上一个 ebp 的值
        uint32_t* last_ebp = (uint32_t*)*ebp;
        
        // 如果下一个 ebp 不在内核空间，或者地址变小了（栈是向下增长的），停止
        // 0xc0100000 是我们在memory.h中定义的内核栈其实地址
        if (last_ebp <= ebp || (uint32_t)last_ebp >= 0xc0100000) {
            break;
        }
        ebp = last_ebp;
    }
}


