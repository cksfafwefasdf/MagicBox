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

/*
+-------------------------------------------------------+ 高地址 (stack_top)
  |  ss                                                   |
  |  esp (用户态栈指针)                                    |  <-- 硬件压入
  |  eflags/ cs/ eip (用户态入口地址)                      |
  |  err_code                                             |
  +-------------------------------------------------------+
  |  ds / es / fs / gs                                    |  <-- 软件 push (intr_stack)
  |  eax / ecx / edx / ebx / esp_dummy / ebp / esi / edi |
  +=======================================================+ <-- 这条线就是 child_intr
  |  func_arg (线程参数)                                  |
  |  function (线程函数)                                  |  <-- 伪造的内核调用结构
  |  unused_retaddr / eip (指向 intr_exit)                |      (thread_stack)
  +-------------------------------------------------------+
  |  esi / edi / ebx / ebp (全为 0)                        |
  +-------------------------------------------------------+ <-- 这条线就是 self_kstack
  |                                                       |
  |  (空闲栈空间，向下生长...)                             	|
  |                                                       |
  v                                                       v 低地址 (kstack_pages)
*/

// 用于处理用户态到内核态的切换
struct intr_stack{
	// 从此处 vec_no 到 ds 的部分由我们软件压入
	// 主要在 kernel.s 中处理，存的是用户态的环境
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
	// 下面的这部分内容（例如 err_code）由硬件自动压入
	uint32_t err_code;
	void (*eip) (void);
	uint32_t cs;
	uint32_t eflags;
	void* esp;
	uint32_t ss;
};

// thread_stack 用于处理内核态到内核态的切换，主要给 switch_to 使用
// 根据 x86 ABI，内核态下调用一个普通的 C 语言函数
// 只需要保存 4 个主叫方保存寄存器（Callee-saved Registers）：esi, edi, ebx, ebp 即可
// 当 switch_to 执行 ret 时，栈顶指针 ESP 正好指向 eip。
// switch_to 的 ret 执行后, CPU 强行把 eip 的值（即 kernel_thread）弹入 EIP 寄存器
// ret 弹栈后，ESP 自动向高地址移动 4 个字节，精准地停在了 unused_retaddr 上
// 此时，CPU 已经进入了 kernel_thread 的代码。作为一个标准的 C 语言函数，查看自己的栈
// [esp] 是 unused_retaddr，它会把这个值当成自己的返回地址。
// [esp + 4] 是 function（第一个参数）
// [esp + 8] 是 func_arg（第二个参数）
// 由于有 unused_retaddr 在这里挡了一下， kernel_thread 寻找参数的相对偏移量（esp + 4 和 esp + 8）就变得完全合法了！
// 由于我们通过 kernel_thread 中的 function(func_arg) 操作，直接就进入到后续的业务逻辑中了
// 因此我们不会从这个桩函数，也就不会用到这个 unused_retaddr 了
// 但是我们并不总是会返回到 kernel_thread 中
// 只有 内核线程 或者调用 process_execute 启动的用户进程（比如 init）才会到 kernel_thread 中
// 因为 thread_start 和 process_execute 底层调用的 thread_create 需要接受一个业务函数以及这个业务函数的参数
// 我们通过 kernel_thread 这个桩可以很好的取出业务函数和参数
// 但是 fork 出来的进程不会到 kernel_thread 中，因为他们没有通过 thread_create 启动
// 我们在 fork 操作的 build_child_stack 函数中直接无视了 function func_arg unused_retaddr 这三个参数
// 直接将 eip 赋值成了 intr_exit，然后将 self_kstack 赋值成了 eip 往下 4 个寄存器，为 esi 等留出了空间
// 到进入 switch_to 时， mov esp,[eax] 操作会更新当前的 esp 为 next 进程的内核栈顶 self_kstack
// 通过一系列 pop 操作后，指针最终就指向了 eip，ret 后就直接进入了 intr_exit
// 经过系统调用的中断返回后，直接进入刚刚父进程进行 fork 的代码的下一行代码
// 对于 thread_create 创建出的进程
// thread_create 函数会将 self_kstack 指向一个完整的 thread_stack 结构体的顶端（最低地址处），因此 pop 后
// 栈顶指向桩函数 kernel_thread
// 桩函数 kernel_thread 通过调用 function(func_arg); 就可以完美的取出 function 和 func_arg 并使用
struct thread_stack{
	uint32_t ebp;
	uint32_t ebx;
	uint32_t edi;
	uint32_t esi;

	void (*eip) (thread_func* func,void* func_arg);

	void (*unused_retaddr)(void);
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

struct file_table {
    int ref_cnt;                            // 引用计数（有多少个线程共享此表）
    struct lock table_lock;                 // 保护动态增删 fd 时的并发锁
    struct fd_entry fd_table[MAX_FILES_OPEN_PER_PROC]; // 实际的打开文件指针数组
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
	struct file_table* file_table;

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
extern void init_file_table(struct file_table* ft);
extern void init_mm_struct(struct mm_struct* mm);
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
