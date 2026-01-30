#ifndef __FS_DIR_H
#define __FS_DIR_H
#include "stdint.h"
#include "stdbool.h"
#include "inode.h"
#include "fs.h"
#include "fs_types.h"

struct dir{
	struct inode* inode;
	// Used to record the offset of the 'cursor' 
	// in the directory while traversing the directory.
	uint32_t dir_pos;
	uint8_t dir_buf[BLOCK_SIZE];
};


extern struct dir root_dir;
extern bool search_dir_entry(struct partition* part,struct dir* pdir,const char* name,struct dir_entry* dir_e);
extern void dir_close(struct dir* dir);
extern struct dir* dir_open(struct partition* part,uint32_t inode_no);
extern void open_root_dir(struct partition* part);
extern void create_dir_entry(char* filename,uint32_t inode_no,uint8_t file_type,struct dir_entry* p_de);
extern bool sync_dir_entry(struct dir* parent_dir,struct dir_entry* p_de,void* io_buf);
extern bool delete_dir_entry(struct partition* part,struct dir* pdir,uint32_t inode_no,void* io_buf);
extern struct dir_entry* dir_read(struct dir* dir);
extern int32_t dir_remove(struct dir* parent_dir,struct dir* child_dir);
extern bool dir_is_empty(struct dir* dir);
extern void close_root_dir(struct partition* part);

#endif