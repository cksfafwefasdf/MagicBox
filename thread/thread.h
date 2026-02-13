#ifndef __THREAD_THREAD_H
#define __THREAD_THREAD_H
#include "stdint.h"
#include "string.h"
#include "memory.h"
#include "bitmap.h"
#include "signal.h"
#include "unistd.h"
#include "fs_types.h"

// each process can open 8 files at most
#define MAX_FILES_OPEN_PER_PROC 8
#define TASK_NAME_LEN 16
#define MAX_PID_NUM 1024
#define MAX_PID_NUM_IN_BYTE MAX_PID_NUM/8
#define STACK_MAGIC 0x20030000

typedef void thread_func(void*);

enum task_status{
	TASK_RUNNING,
	TASK_READY,
	TASK_BLOCKED,
	TASK_WAITING,
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

struct task_struct{
	uint32_t* self_kstack;
	pid_t pid;
	enum task_status status;
	char name[TASK_NAME_LEN];
	uint8_t priority;
	uint8_t ticks;
	uint32_t elapsed_ticks;

	// Per-process Open File Table
	int32_t fd_table[MAX_FILES_OPEN_PER_PROC];

	struct dlist_elem general_tag;
	struct dlist_elem all_list_tag;

	pid_t pgrp; // 进程组id，初始情况下进程的组id就是自己的pid

	uint32_t* pgdir;
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
	// 结果内核立刻带你去执行 4 号信号的处理函数。
	// 该信号可以用于进行一些临界区数据保护操作
    uint32_t blocked; // 信号屏阻塞位图
    struct sigaction sigactions[SIG_NR]; // 信号执行属性结构，对应信号将要执行的操作和标志信息。 

	uint32_t alarm; // 报警定时值（滴答数），用于定时发送 SIGALRM 信号

	// 内存布局边界
	// 保护模式下，暴露给用户的地址都是虚拟地址，因此这里用的也都是虚拟地址
	// 我们的系统是现代平坦模型（所有段基址都是 0，直接映射 4GB 虚拟空间）
	// 因此我们这些字段的含义与linux早期版本中的不太一样，linux早期版本中这些字段存的都是段的偏移量
	// 而我们直接存虚拟地址
	// 这些地址都是在 load 函数中填写的
	// 而 load 函数又是由 execv 调用的
	// 这意味着我们的这些字段只有用户进程会去填充，内核进程是不会去填充的
	// 这是合理的，因为内核线程通常直接运行在内核地址空间（3GB 以上）
	// 它们没有自己的用户态虚拟地址池（userprog_vaddr 为空），也没有 ELF 文件。
	// 并且内核线程申请内存用的是 kmalloc（在内核页表里分配），它们不使用 sys_brk。
    uint32_t start_code; // 代码段起始地址
    uint32_t end_code; // 代码段结束地址
    uint32_t start_data; // 数据段起始地址
    uint32_t end_data; // 数据段结束地址（也是堆的起始地址 start_brk）
	// 实际上，我们使用堆的起始加上虚拟地址位图的最后一位也能算出堆顶
	// 但是有一种特殊情况，例如堆里可能有“空洞”，例如 1111001111
	// 如果只靠位图找 brk，你会把中间那个 0 的位置误认为堆顶，导致地址空间分配冲突。
	// 除非你去扫描整个位图，但是每次都扫描整个位图太费时间了，brk相当于是一个“缓存”
    uint32_t brk; // 当前堆顶（sbrk 操纵的对象）
    uint32_t start_stack; // 用户栈底地址

	// 挂载该进程管理的 vm_area
	// 使用侵入式链表定义，这样的话thread.h就不用抱包含vma.h了
	// 避免了循环依赖
	struct dlist vma_list;

	struct virtual_addr userprog_vaddr;
	struct mem_block_desc u_block_desc[DESC_TYPE_CNT];
	uint32_t cwd_inode_nr;
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

extern struct dlist thread_ready_list;
extern struct dlist thread_all_list; // queue of all tasks


#endif