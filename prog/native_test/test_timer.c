#include <stdio.h>
#include <syscall.h> 

int main(int argc, char** argv) {
    printf("--- MagicBox Timer Logic Test ---\n");

    // 验证 msleep 是否真的阻塞
    printf("Test 1: Start 2s sleep...\n");
    msleep(2000); 
    printf("Test 1: Wake up! (If this shows quickly, sleep is broken)\n");

    // 验证 alarm 覆盖逻辑
    printf("\nTest 2: Setting 10s alarm, then immediately reset to 2s...\n");
    alarm(10);
    alarm(2); // 应该覆盖上面的 10s，2s后退出
    
    // 进入一个比闹钟长的睡眠
    // 如果逻辑正确，2秒后闹钟响，进程退出，下面这行不会打印
    msleep(5000); 
    
    printf("Test 2 Failed: I should be dead by now!\n");

    return 0;
}