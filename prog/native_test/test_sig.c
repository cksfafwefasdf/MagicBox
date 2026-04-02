#include <stdio.h>
#include <syscall.h>
#include <string.h>
#include <stdint.h>

#define SIGPIPE 13

// 测试非法指令
static void sigill_test(){
    __asm__ volatile ("hlt"); // 用户态执行关机指令，也会触发异常
}

// 测试非法访问 SIGSEGV
static void sigsegv_test(){
    int *a = 0x0;
	*a = 3;
}


static void my_sig_handler(int sig) {
    // 真实的 handler 里用 printf 是不安全的，但测试时先这么干吧
    printf("\n[Kernel Test] Process caught SIGPIPE (%d)!\n", sig);
}

// 检验管道破裂，以及信号 SIGPIPE 以及 自定义信号 handler  同时可以测试init进程是否可以正确回收孤儿进程
static void sigpipe_test(){
    // int fd[2];
    // 测试用户态 malloc
    int* fd = malloc(sizeof(2));
    pipe(fd);

    // 注册处理函数，这样进程就不会被默认动作杀掉了
    signal(SIGPIPE, my_sig_handler);

    if (fork() == 0) {
        close(fd[0]);
        printf("Child: Closed read end and exiting...\n");
        exit(0);
    }

    close(fd[0]);
    for(volatile uint32_t i = 0; i < 1000000000; i++); 

    printf("Parent: Attempting to write...\n");
    int ret = write(fd[1], "test", 4);

    // 因为信号被捕获了，系统调用会返回错误码，进程继续执行
    printf("Parent: write returned %d\n", ret); 
    
    // 此时父进程还活着，它可以顺手回收一下儿子
    // wait(NULL); 
    free(fd);
    
}

int main(int argc, char** argv){
    if(argc!=2){
        printf("sig_test must has 2 arguments!\n");
        return 0;
    }

    if (!strcmp("ill",argv[1])) {
        sigill_test();
    }else if(!strcmp("pipe",argv[1])){
        sigpipe_test();
    }else if(!strcmp("segv",argv[1])){
        sigsegv_test();
    }
}