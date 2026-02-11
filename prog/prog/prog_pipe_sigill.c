#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdint.h"

// 测试非法指令

int main() {
	__asm__ volatile ("hlt"); // 用户态执行关机指令，也会触发异常
    return 0;
}