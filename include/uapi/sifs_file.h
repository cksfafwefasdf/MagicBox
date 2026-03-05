#ifndef __INCLUDE_UAPI_SIFS_FILE_H
#define __INCLUDE_UAPI_SIFS_FILE_H

#include "stdint.h"
#include "unistd.h"
#include "fs_types.h"

struct partition;

extern void bitmap_sync(struct partition* part,uint32_t bit_idx,enum bitmap_type btmp_type);
extern int32_t block_bitmap_alloc(struct partition* part);
extern int32_t inode_bitmap_alloc(struct partition* part);
extern int32_t sifs_file_create(struct inode* parent_inode, char* filename, uint8_t flag);
extern int32_t file_write(struct file* file,const void* buf,uint32_t count);
extern int32_t file_close(struct file* file);
extern int32_t file_open(struct partition* part,uint32_t inode_no,uint8_t flag);
extern int32_t file_read(struct file* file,void* buf,uint32_t count);

// extern uint32_t prog_size; // the size of the program being executed
#endif