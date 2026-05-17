#ifndef __INCLUDE_MAGICBOX_FORK_H
#define __INCLUDE_MAGICBOX_FORK_H
#include <stdint.h>

#define CLONE_VM    0x00000100  // 共享内存地址空间（内核线程的核心，用于共享相同的地址空间）
#define CLONE_FS	0x00000200	// set if fs info shared between processes
#define CLONE_FILES 0x00000400  // 共享打开文件表

extern pid_t sys_clone(uint32_t flags, void* user_stack, int (*fn)(void *fnarg), void *arg, void (*thread_restorer)(void));
extern pid_t sys_fork(void);

#endif