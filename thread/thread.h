#ifndef __THREAD_THREAD_H
#define __THREAD_THREAD_H
#include "../lib/stdint.h"
#include "../lib/string.h"
#include "../kernel/memory.h"
#include "../lib/kernel/bitmap.h"
// each process can open 8 files at most
#define MAX_FILES_OPEN_PER_PROC 8
#define TASK_NAME_LEN 16
#define MAX_PID_NUM 1024
#define MAX_PID_NUM_IN_BYTE MAX_PID_NUM/8

typedef void thread_func(void*);
typedef int16_t pid_t;

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

	uint32_t* pgdir;
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

extern struct dlist thread_ready_list;
extern struct dlist thread_all_list; // queue of all tasks


#endif