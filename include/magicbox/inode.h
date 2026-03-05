#ifndef __INCLUDE_MAGICBOX_INODE_H
#define __INCLUDE_MAGICBOX_INODE_H

#include "stdint.h"

/*
    这个文件中的代码都是虚拟文件系统VSF中，通用的inode操作的代码
*/

struct partition;
struct inode;

extern void inode_close(struct inode* inode);
extern struct inode* inode_open(struct partition* part,uint32_t inode_no);
extern int32_t inode_register_to_cache(struct inode* inode);
extern void inode_cache_init(void);
#endif