#ifndef __INCLUDE_MAGICBOX_INODE_H
#define __INCLUDE_MAGICBOX_INODE_H

#include <stdint.h>
#include <hashtable.h>
#include <sync.h>

/*
    这个文件中的代码都是虚拟文件系统VSF中，通用的inode操作的代码
*/

enum time_flag {
    ATIME = 1,
    MTIME = 2,
    CTIME = 4,
};

struct partition;
struct inode;

extern void inode_close(struct inode* inode);
extern struct inode* inode_open(struct partition* part,uint32_t inode_no);
extern int32_t inode_register_to_cache(struct inode* inode);
extern void inode_cache_init(void);
extern struct inode* make_anonymous_inode(void);
extern int32_t inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count);
extern void inode_evict(struct inode* inode);
extern enum file_types decode_imode(uint16_t mode);
extern uint16_t encode_imode(enum file_types ft,uint16_t mode);
extern void update_time(struct inode* inode, int flags);

#endif