#ifndef __FS_FS_H
#define __FS_FS_H

#include "stdint.h"
#include "ide.h"
#include "fs_types.h"

// the max number of the file in each partitions is 4096
#define MAX_FILES_PER_PART 4096
// each sector has 512 B, which equals to 512*8=4096 b
#define BITS_PER_SECTOR 4096 

#define ADDR_BYTES_32BIT 4 // size of the 32 bits address pointor

#define FS_MAGIC_NUMBER 0x20030000 // magic number for this file system

struct path_search_record{
	char searched_path[MAX_PATH_LEN];
	struct dir* parent_dir;
	enum file_types file_type;
};

extern void filesys_init(void);
extern int32_t sys_open(const char* pathname,uint8_t flags);
extern int32_t path_depth_cnt(char* pathname);
extern int32_t sys_write(int32_t fd,const void* buf,uint32_t count);
extern int32_t sys_close(int32_t fd);
extern int32_t sys_read(int32_t fd,void* buf,uint32_t count);
extern int32_t sys_lseek(int32_t fd,int32_t offset,enum whence whence);
extern int32_t sys_unlink(const char* pathname);
extern int32_t sys_mkdir(const char* pathname);
extern int32_t sys_closedir(int32_t fd_dir);
extern int32_t sys_opendir(const char* name);
extern void sys_rewinddir(int32_t fd);
extern int32_t sys_readdir(int32_t fd, struct dir_entry* de);
extern int32_t sys_rmdir(const char* pathname);
extern int32_t sys_chdir(const char* path);
extern char* sys_getcwd(char* buf,uint32_t size);
extern int32_t sys_stat(const char* path,struct stat* buf);
extern char* _path_parse(char* pathname,char* name_store);
extern uint32_t fd_local2global(uint32_t local_fd);
extern void sys_disk_info(void);
extern void sys_mount(const char* part_name);

extern struct partition* cur_part;

#endif