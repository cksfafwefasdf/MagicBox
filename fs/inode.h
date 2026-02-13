#ifndef __FS_INODE_H
#define __FS_INODE_H
#include "stdint.h"
#include "stdbool.h"
#include "unistd.h"
#include "fs_types.h"

struct partition;

extern void inode_init(struct partition* part, uint32_t inode_no,struct m_inode* new_inode,enum file_types ft);
extern void inode_close(struct m_inode* inode);
extern struct m_inode* inode_open(struct partition* part,uint32_t inode_no);
extern void inode_sync(struct partition* part,struct m_inode* inode,void* io_buf);
extern void inode_release(struct partition* part,uint32_t inode_no);
extern void inode_delete(struct partition* part,uint32_t inode_no,void* io_buf);
extern struct m_inode* make_anonymous_inode(void);
extern int32_t inode_read_data(struct m_inode* inode, uint32_t offset, void* buf, uint32_t count);

#endif