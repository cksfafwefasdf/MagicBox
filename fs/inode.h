#ifndef __FS_INODE_H
#define __FS_INODE_H
#include "../lib/stdint.h"
#include "../lib/stdbool.h"
#include "../lib/kernel/dlist.h"
#include "fs.h"

#define DIRECT_INDEX_BLOCK 12
#define FIRST_LEVEL_INDEX_BLOCK 1

#define BLOCK_PTR_NUMBER DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK

#define TOTAL_BLOCK_COUNT (DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK*(BLOCK_SIZE/ADDR_BYTES_32BIT))

struct inode{
	uint32_t i_no;
	// when inode points to file, i_size is the size of the file
	// when inode points to dict file, i_size is the size of the sum of the dict entry
	uint32_t i_size;
	// how many times this file is opened 
	uint32_t i_open_cnts;
	// write operation will cause concurrent safty problem
	// so make write_deny true, before write the file. 
	bool write_deny;
	// 0~11 are direct pointors, 12 is the level-one indirect pointor 
	uint32_t i_sectors[BLOCK_PTR_NUMBER];
	// this tag is used for the 'already opened inode queue'
	// to prevent redundant reads of inodes from the disk.
	struct dlist_elem inode_tag;
};

extern void inode_init(uint32_t inode_no,struct inode* new_inode);
extern void inode_close(struct inode* inode);
extern struct inode* inode_open(struct partition* part,uint32_t inode_no);
extern void inode_sync(struct partition* part,struct inode* inode,void* io_buf);
extern void inode_release(struct partition* part,uint32_t inode_no);
extern void inode_delete(struct partition* part,uint32_t inode_no,void* io_buf);

#endif