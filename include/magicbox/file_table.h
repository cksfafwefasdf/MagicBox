#ifndef __FS_FILE_TABLE_H
#define __FS_FILE_TABLE_H

#include "stdint.h"
#include "fs_types.h"

/*
    该文件是针对全局打开文件表的一些操作
    由于全局打开文件表应该是要所有文件系统都能访问到的
    因此他必须和文件系统无关，必须得抽离出来
    抽离出来这个文件还有一个好处，原本我们的 file_table 是在 sifs_file.h 中的
    而 wait exit 这些调用都要用到 file_table
    这就导致这些文件都需要包含与文件系统强相关的 sifs_file.h, 耦合性太高了！
    抽离出来后，现在这些文件只需要包含和 VFS 相关的 fs_types 以及和具体文件系统无关的 file_table.h 即可
*/ 

#define MAX_FILE_OPEN_IN_SYSTEM 32

struct task_struct;

extern int32_t pcb_fd_install(int32_t global_fd_idx);
extern int32_t get_free_slot_in_global(void);
extern uint32_t fd_local2global(struct task_struct* task , uint32_t local_fd);

extern struct file file_table[MAX_FILE_OPEN_IN_SYSTEM];

#endif