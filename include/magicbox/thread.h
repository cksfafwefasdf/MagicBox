#ifndef __INCLUDE_MAGICBOX_THREAD_H
#define __INCLUDE_MAGICBOX_THREAD_H
#include <stdint.h>
#include <string.h>
#include <memory.h>
#include <bitmap.h>
#include <signal.h>
#include <unitype.h>

// each process can open 8 files at most
#define MAX_FILES_OPEN_PER_PROC 32
#define TASK_NAME_LEN 64
#define MAX_PID_NUM 1024
#define MAX_PID_NUM_IN_BYTE MAX_PID_NUM/8
#define STACK_MAGIC 0x20030000
#define KERNEL_THREAD_STACK_PAGES 2
#define KERNEL_THREAD_STACK PG_SIZE*KERNEL_THREAD_STACK_PAGES
// 用于提取8KB之后的那些页号
#define KERNEL_THREAD_STACK_MASK 0xffffe000

#define INIT_PID 1

struct inode;


typedef void thread_func(void*);

enum task_status{
	TASK_RUNNING,
	TASK_READY,
	TASK_BLOCKED, // 不可中断阻塞，给磁盘 io 等操作使用
	TASK_WAITING, // 可中断阻塞，给 wait poll read 等函数使用
	TASK_HANGING,
	TASK_DIED
};

struct intr_stack{
	uint32_t vec_no;
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t esp_dummy;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t gs;
	uint32_t fs;
	uint32_t es;
	uint32_t ds;

	uint32_t err_code;
	void (*eip) (void);
	uint32_t cs;
	uint32_t eflags;
	void* esp;
	uint32_t ss;
};

struct thread_stack{
	uint32_t ebp;
	uint32_t ebx;
	uint32_t edi;
	uint32_t esi;

	void (*eip) (thread_func* func,void* func_arg);

	void (*unused_retaddr);
	thread_func* function;
	void* func_arg;
};

// FD_CLOEXEC 是 File Descriptor Close-on-Exec 的缩写
// 表示当进程执行 exec 系列系统调用启动新程序时，自动关闭这个文件描述符
// 在没有这个标志位之前，我们必须在 fork() 之后、exec() 之前，
// 手动写一堆 close(fd) 来清理不需要的文件。但这存在竞态条件，用了这个标志就会好很多
struct fd_entry {
    int32_t global_fd_idx;  // 指向全局 file_table 的索引
    uint8_t flags;          // 记录 FD_CLOEXEC 等
};

struct task_struct{
	uint32_t* self_kstack; // 动态栈顶（给 switch_to 用）
    void* kstack_pages; // 栈的基地址（给 free 用）
	pid_t pid;
	enum task_status status;
	char name[TASK_NAME_LEN];
	int16_t priority;
	int16_t ticks;
	bool is_dyn_link; // 标志当前进程是否是动态链接的，用于优化 swap
	uint32_t elapsed_ticks;

	// Per-process Open File Table
	// struct fd_entry fd_table[MAX_FILES_OPEN_PER_PROC];
	struct fd_entry* fd_table;

	struct dlist_elem general_tag;
	struct dlist_elem all_list_tag;
	struct dlist_elem timer_tag;
	struct dlist_elem alarm_tag;

	pid_t pgrp; // 进程组id，初始情况下进程的组id就是自己的pid

	struct mm_struct* mm;        // 指向进程的内存描述符。如果是内核线程，此项为 NULL

	// 不可靠信号，由于我们是用位图来区分是否存在信号的，其值只能是0和1
	// 若同时接收到三个 SIGINT，我们显然只会去处理一次这个信号，另外两次就会丢掉
	// 因此这被称为不可靠信号，这是 0.12 版本 linux 所实现的信号 
	uint32_t signal; // 信号位图
	// 该 blocked 位图用于阻塞信号以便于稍后运行，例如
	// 开始处理 3 号信号时，进入 3 号信号的处理函数。
	// 设置屏蔽，我们通过 sigprocmask 把 blocked 位的 4 和 5 置为 1。
	// 之后信号 4 来了，内核调用 sig_addset(&cur->signal, 4)。
	// 但由于 do_signal 里有 pending = cur->signal & ~cur->blocked，计算结果发现 4 号位是 0。内核什么都不做，直接返回用户态。
	// 此时信号 4 依然在 cur->signal 位图里“待命”
	// 处理完 3 号信号后，函数逻辑结束，准备退出。
	// 恢复屏蔽，把 blocked 位恢复，4 和 5 变回 0。
	// 之后瞬间触发，就在恢复屏蔽后的下一次中断（比如时钟中断）发生时，
	// do_signal 再次运行。此时 pending = cur->signal & ~cur->blocked 的计算结果中，4 号位变成了 1。
	// 结果内核立刻带我们去执行 4 号信号的处理函数。
	// 该信号可以用于进行一些临界区数据保护操作
    uint32_t blocked; // 信号屏阻塞位图
    struct sigaction sigactions[SIG_NR]; // 信号执行属性结构，对应信号将要执行的操作和标志信息。 

	uint32_t alarm; // 报警定时值（滴答数），用于定时发送 SIGALRM 信号
	uint32_t wait_until; // 用于 sleep 或者 poll 等操作，该操作不会涉及到信号的操作

	// struct virtual_addr userprog_vaddr;
	struct inode* pwd; // 进程的当前工作目录
	int16_t parent_pid;
	int8_t exit_status;
	uint32_t stack_magic;
};


extern void init_thread(struct task_struct* pthread,char* name,int prio);
extern void thread_create(struct task_struct* pthread,thread_func* function,void* func_arg);
extern struct task_struct* thread_start(char* name,int prio,thread_func function,void* func_arg);
extern struct task_struct* get_running_task_struct(void);
extern void schedule(void);
extern void thread_environment_init(void);
extern void thread_block(enum task_status stat);
extern void thread_unblock(struct task_struct* pthread);
extern void thread_yield(void);
extern pid_t fork_pid(void);
extern void sys_ps(void);
extern void thread_exit(struct task_struct* thread_over,bool need_schedule);
extern struct task_struct* pid2thread(int32_t pid);
extern void release_pid(pid_t pid);
extern pid_t sys_setpgid(pid_t pid, pid_t pgid);
extern pid_t sys_getpgid(pid_t pid);
extern pid_t sys_getppid(void);

extern struct dlist thread_ready_list;
extern struct dlist thread_all_list; // queue of all tasks

#endif
