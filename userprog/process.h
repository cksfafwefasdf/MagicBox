#ifndef __USERPROG_PROCESS_H
#define __USERPROG_PROCESS_H

#include "stdint.h"

struct task_struct;

#define USER_VADDR_START 0x8048000
#define USER_STACK3_VADDR (0xc0000000-0x1000)
#define DEFAULT_PRIO 31
extern void start_process(void* filename_);
extern void page_dir_activate(struct task_struct* pthread);
extern void process_activate(struct task_struct* pthread);
extern uint32_t* create_page_dir(void);
extern void create_user_vaddr_bitmap(struct task_struct* user_prog);
extern void process_execute(void* filename,char* name);
#endif