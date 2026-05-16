#ifndef __INCLUDE_MAGICBOX_FORK_H
#define __INCLUDE_MAGICBOX_FORK_H
#include <stdint.h>

#define CLONE_VM    0x00000100  // 共享内存地址空间（内核线程的核心，用于共享相同的地址空间）
#define CLONE_FILES 0x00000400  // 共享打开文件表

extern pid_t sys_fork(void);

#endif