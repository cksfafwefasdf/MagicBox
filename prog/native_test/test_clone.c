#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <syscall.h>

#define CLONE_VM    0x00000100  // 共享内存地址空间（内核线程的核心，用于共享相同的地址空间）
#define CLONE_FS	0x00000200	// set if fs info shared between processes
#define CLONE_FILES 0x00000400  // 共享打开文件表

// 分配给子线程的独立用户栈大小
#define THREAD_STACK_SIZE 4096
uint8_t thread_stack[THREAD_STACK_SIZE];

// 全局变量：用于测试内存共享 (CLONE_VM)
volatile int global_shared_counter = 100;

// 子线程的入口函数
int32_t test_lwp_entry(void* arg) {
    printf("[Child LWP] Started. Arg received: %d\n", (int)arg);
    
    // 验证栈独立性：在自己的栈上操作
    volatile int local_var = 0x55aa;
    printf("[Child LWP] Local variable address: 0x%x, value: 0x%x\n", &local_var, local_var);

    // 验证内存共享：修改全局变量
    printf("[Child LWP] Modifying global_shared_counter from %d to 999...\n", global_shared_counter);
    for(volatile int j = 0; j < 50; j++){
        for(volatile int i = 0; i < 100000000; i++);
    }
        
    global_shared_counter = 999;

    // 验证文件表共享：假设 fd 1 是 stdout
    // 如果 CLONE_FILES 正常，子线程向 1 写数据，控制台能正常打印
    printf("[Child LWP] Testing CLONE_FILES by writing to stdout...\n");

    printf("[Child LWP] Exiting...\n");
    return 32;
}

int main() {
    printf("[Parent] Starting LWP clone test...\n");
    printf("[Parent] Initial global_shared_counter = %d\n", global_shared_counter);

    // 计算子线程的栈顶。
    // x86 的栈是向低地址增长的(Full Descending)，所以必须传入数组的高地址端！
    void* child_stack_top = (void*)((uint32_t)thread_stack + THREAD_STACK_SIZE);
    
    printf("[Parent] Ready to clone. Child stack top: 0x%x\n", child_stack_top);

    int pid = pthread_create(child_stack_top, test_lwp_entry, (void*)42);

    if (pid < 0) {
        printf("[Parent] ERROR: Clone failed!\n");
        return 0;
    }
    
    // 父进程逻辑
    printf("[Parent] Cloned child LWP successfully. Child PID = %d\n", pid);
    printf("[Parent] Check pid: %d\n", getpid());
    
    // 故意阻塞或循环等待一段时间，给子线程充分的运行时间
    // 在实际开发中，如果实现了内核级睡眠，可以用 sleep。这里用空循环模拟
    printf("[Parent] Waiting for child LWP to modify memory...\n");
    for (volatile int j = 0; j < 1000; j++) {
        for (volatile int i = 0; i < 500000000; i++) {
            if (global_shared_counter == 999) {
                break; // 检测到子线程修改了，提前退出
            }
        }
    }
    
    // 验证结果
    printf("[Parent] Inspection: global_shared_counter = %d\n", global_shared_counter);
    if (global_shared_counter == 999) {
        printf("[Result] SUCCESS! CLONE_VM is working perfectly.\n");
    } else {
        printf("[Result] FAIL! Parent did not see child's modification.\n");
    }

    // 回收子线程挂起状态
    int status = 0;
    int recycled_pid = waitpid(pid, &status, 0);
    printf("[Parent] sys_waitpid recycled PID: %d, status: %d\n", recycled_pid, status);

    return 0;
}