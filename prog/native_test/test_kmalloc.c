#include <syscall.h>
#include <stdio.h>

// 用来测试 kmalloc 和 kfree
// 他的主要用途就是一个用来触发 sys_test 调用的启动器
int main(void) {
    printf("test_kmalloc: invoke kernel kmalloc test\n");
    test_func();
    printf("test_kmalloc: done\n");
    return 0;
}
