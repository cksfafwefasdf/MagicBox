#ifndef __USERPROG_PROCESS_H
#define __USERPROG_PROCESS_H

#include "stdint.h"

struct task_struct;

// 我们将用户栈栈底设置在用户虚拟地址的最高地址处
// 初始时，我们为其分配了一个页的栈空间，所以此处要减去0x1000
#define USER_STACK3_VADDR (0xc0000000-0x1000)
#define DEFAULT_PRIO 31
extern void start_process(void* filename_);
extern void page_dir_activate(struct task_struct* pthread);
extern void process_activate(struct task_struct* pthread);
extern uint32_t* create_page_dir(void);
extern void create_user_vaddr_bitmap(struct task_struct* user_prog);
extern void process_execute(void* filename,char* name);
extern void release_pg_block(struct task_struct* task);
extern void release_pg_table(struct task_struct* task);
extern void release_pg_dir(struct task_struct* task);
extern void user_vaddr_space_clear(struct task_struct* cur);
#endif