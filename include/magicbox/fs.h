#ifndef __INCLUDE_MAGICBOX_FS_H
#define __INCLUDE_MAGICBOX_FS_H

#include <stdint.h>
#include <unistd.h>

#define SIFS_FS_MAGIC_NUMBER 0x20030000 // magic number for this file system

extern void filesys_init(void);
extern int32_t sys_open(const char* pathname,uint8_t flags);
extern int32_t path_depth_cnt(char* pathname);
extern int32_t sys_write(int32_t fd,void* buf,uint32_t count);
extern int32_t sys_close(int32_t fd);
extern int32_t sys_read(int32_t fd,void* buf,uint32_t count);
extern int32_t sys_lseek(int32_t fd,int32_t offset,enum whence whence);
extern int32_t sys_unlink(const char* pathname);
extern int32_t sys_mkdir(const char* pathname);
// 对于文件夹的 open 和 close 统一放到 sys_open 和 sys_close 中
// 校验文件夹只能以只读方式打开即可
// extern int32_t sys_closedir(int32_t fd_dir);
// extern int32_t sys_opendir(const char* name);
extern void sys_rewinddir(int32_t fd);
extern int32_t sys_readdir(int32_t fd, struct dirent* de);
extern int32_t sys_rmdir(const char* pathname);
extern int32_t sys_chdir(const char* path);
extern char* sys_getcwd(char* buf,uint32_t size);
extern int32_t sys_stat(const char* path,struct stat* buf);
extern char* _path_parse(char* pathname,char* name_store);
extern void sys_disk_info(void);
extern void sys_mount(const char* part_name);
extern int32_t sys_dup2(uint32_t old_local_fd, uint32_t new_local_fd);
extern int32_t sys_mknod(const char* pathname, enum file_types type, uint32_t dev);
extern void make_dev_nodes(void);
extern int32_t sys_mkfifo(const char* pathname);

extern struct partition* cur_part;

#endif