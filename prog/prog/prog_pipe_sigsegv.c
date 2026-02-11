#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include "stdint.h"

// 测试非法访问 SIGSEGV

int main() {
    int *a = 0x0;
	*a = 3;
    return 0;
}