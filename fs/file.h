#ifndef __FS_FILE_H
#define __FS_FILE_H

#include "../lib/stdint.h"
#include "../device/ide.h"
#include "dir.h"

#define MAX_FILE_OPEN_IN_SYSTEM 32

struct file{
	uint32_t fd_pos;
	uint32_t fd_flag;
	struct inode* fd_inode;
};

enum std_fd{
	stdin_no,
	stdout_no,
	stderr_no
};

enum bitmap_type{
	INODE_BITMAP,
	BLOCK_BITMAP
};

extern void bitmap_sync(struct partition* part,uint32_t bit_idx,enum bitmap_type btmp_type);
extern int32_t block_bitmap_alloc(struct partition* part);
extern int32_t inode_bitmap_alloc(struct partition* part);
extern int32_t pcb_fd_install(int32_t global_fd_idx);
extern int32_t get_free_slot_in_global(void);
extern int32_t file_create(struct dir* parent_dir,char* filename,uint8_t flag);
extern int32_t file_write(struct file* file,const void* buf,uint32_t count);
extern int32_t file_close(struct file* file);
extern int32_t file_open(uint32_t inode_no,uint8_t flag);
extern int32_t file_read(struct file* file,void* buf,uint32_t count);

extern struct file file_table[MAX_FILE_OPEN_IN_SYSTEM];
#endif