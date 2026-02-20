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
#include "vma.h"
void init(void);
void print_logo(void);
static void after_init(void);
static void recyle_idle(void);
// linux 标准中，argv 必须以 NULL 结尾
char* argv[] = {SHELL_PATH,NULL} ;

struct task_struct* main_thread;
struct task_struct* idle_thread;

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
        ioctl(0, TIOCSPGRP, (uint32_t)&ret_pid);

        int status;
        int child_pid;
        while (1) {
            child_pid = wait(&status);
            printf("I'm init, my pid is %d, I receive a child, it's pid is %d, status is %d\n",INIT_PID, child_pid, status);
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
        execv(SHELL_PATH, (const char **)argv);

        // 如果 execv 返回了，说明执行失败
        panic("init: shell execv failed!");
    }
    panic("init: should not be here!\n");
}

void print_logo(){
    char* jester[] = {
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

// run the idle thread when system is not busy 
static void idle(void *arg UNUSED){
	while (1){

		thread_block(TASK_BLOCKED);
		asm volatile("sti;hlt":::"memory");
	}
}

static void make_main_thread(void) {
    main_thread = get_kernel_pages(1);
    init_thread(main_thread, "main", 5);
    
    
    // 准备新栈，：将原本要执行的后续代码after_init手动压入新栈
    uint32_t* esp = (uint32_t*)((uint32_t)main_thread + PG_SIZE);
    *(--esp) = (uint32_t)after_init; // 告诉 CPU 以后去哪

    ASSERT(!dlist_find(&thread_all_list,&main_thread->all_list_tag));
	dlist_push_back(&thread_all_list,&main_thread->all_list_tag);
    
    // 切换
    asm volatile ("movl %0, %%esp; ret" : : "g"(esp) : "memory");
}

static void recyle_idle(){
    intr_disable(); // 变身期间禁止干扰
    if (idle_thread != NULL) {
        release_pid(idle_thread->pid);
        dlist_remove(&idle_thread->all_list_tag);
        if(dlist_find(&thread_ready_list,&idle_thread->general_tag)){
            dlist_remove(&idle_thread->general_tag); 
        }
        mfree_page(PF_KERNEL,idle_thread,1); // 释放 PCB 页面
    }

    // 身份接管成为新的idle
    idle_thread = main_thread; 
    strcpy(main_thread->name, "idle"); // 改个名字，方便 ps 查看
    intr_enable();
}

static void after_init() {
    // 这里才是主线程真正的逻辑起点
    // 此时它已经站在全新的、动态分配的 PCB 顶端了
    idle_thread = thread_start("idle",3,idle,NULL);
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
    // 先回收idle，使得并使得main变成idle
    recyle_idle();
    put_str("init all done...\n");
    print_logo();
    put_str("start init...\n");
    process_execute(init,"init");
    
    // 启动新进程后，main进程可以放心进入 idle 逻辑了
    idle(NULL);
}

void early_init(void){
    put_str("init all start...\n");
    intr_init();
    mem_init();
    thread_environment_init();
    // 由于 make_main_thread 会重新构建main进程的内核栈
    // 因此原本的函数调用信息会全部丢失，我们不太好再返回到early_init了
    // 因此我们直接控制主线程在make_main_thread的最后，直接将流程转到after_init
    make_main_thread();
}