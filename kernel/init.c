#include "init.h"
#include "interrupt.h"
#include "print.h"
#include "timer.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "keyboard.h"
#include "tss.h"
#include "syscall-init.h"
#include "ide.h"
#include "fs.h"
#include "ide_buffer.h"
#include "process.h"
#include "syscall.h"
#include "assert.h"
#include "global.h"
#include "color.h"
#include "tar.h"
#include "tty.h"
#include "stdio.h"
#include "device.h"
#include "debug.h"
void init(void);
void print_logo(void);

void init(void) {
    // 先打开一个可读可写的控制台
    int32_t tty_fd = open("/dev/tty0", O_RDWR);
    
    // 强制把 console_fd 拷贝到 0, 1, 2 号 FD
    dup2(tty_fd, 0); // stdin
    dup2(tty_fd, 1); // stdout
    dup2(tty_fd, 2); // stderr
    
    if (tty_fd > 2) close(tty_fd);
    
    printf("process init started...\n");

    int32_t ret_pid = fork();
    
    if (ret_pid) { // 父进程逻辑：负责回收僵尸进程
        // init 第一个启动的进程是shell，强制将shell的进程组设置为自己的pid
        setpgid(ret_pid,ret_pid);
        // 将shell的进程组设置为tty的台前进程组
        // 0, 1, 2 都是 tty 的描述符，用绑定一个
        ioctl(0, TIOCSPGRP, &ret_pid);

        int status;
        int child_pid;
        while (1) {
            child_pid = wait(&status);
            printf("I'm init, my pid is 3, I receive a child, it's pid is %d, status is %d\n", child_pid, status);
        }
    } else { // 子进程逻辑：启动 Shell

        setpgid(0, 0); // 确保自己进组，双重确认

        // 检查 Shell 是否已经存在于文件系统中
        int fd = open(SHELL_PATH, O_RDONLY);

        if (fd == -1) {
            // 如果 Shell 不存在，说明是首次启动或文件系统损坏，执行全量恢复
            printf("init: shell not found. Unpacking system files from LBA 1000...\n");
            
            // 执行解压函数
            // 来自动解析 tar 包，并按需调用 readraw 和 mkdir
            untar_all(APPS_LBA); 

            // 解压完成后，再次确认 Shell 是否成功创建
            fd = open(SHELL_PATH, O_RDONLY);
            if (fd == -1) {
                printf("init: critical error! Shell extraction failed.\n");
                while(1); // 挂起，以便查看错误日志
            }
        }
        
        // 运行 Shell
        close(fd);
        printf("init: starting shell %s...\n", SHELL_PATH);
        execv(SHELL_PATH, NULL);

        // 如果 execv 返回了，说明执行失败
        panic("init: shell execv failed!");
    }
    panic("init: should not be here!\n");
}

void print_logo(){
    const char* jester[] = {
        RED"    _________    ",
        "   /         \\   ",
        "  |  \\( o o )/|",
        "  |   \\  \"  / |",
        "  |    )---(  |",
        "   \\  /     \\ |",
        "    \\/_______\\/ "RESET
    };
    int i=0;
    for (; i < 7; i++) {
        console_put_str(jester[i]);console_put_char('\n');
    }
}

void init_all(void){
    put_str("init all start...\n");
    intr_init();
    mem_init();
    thread_environment_init();
    timer_init();
    console_init();
    tty_init();
    keyboard_init();
    tss_init();
    syscall_init();
    intr_enable(); // ide_init will use the interrupt
    ide_buffer_init();
    ide_init();
    filesys_init();
    make_dev_nodes();
    put_str("init all done...\n");
    print_logo();
    put_str("start init...\n");
    process_execute(init,"init");
}