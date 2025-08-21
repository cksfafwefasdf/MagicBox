#ifndef __LIB_USER_SYSCALL_H
#define __LIB_USER_SYSCALL_H
#include "../stdint.h"
#include "../thread/thread.h"
#include "../../fs/fs.h"
// syscall.h includes interface for user

enum SYSCALL_NR {
	SYS_GETPID,
	SYS_WRITE,
	// SYS_WRITE_INT,
	SYS_MALLOC,
	SYS_FREE,
	SYS_FORK,
	SYS_READ,
	SYS_PUTCHAR,
	SYS_CLEAR,
	SYS_GETCWD,
	SYS_OPEN,
	SYS_CLOSE,
	SYS_LSEEK,
	SYS_UNLINK,
	SYS_MKDIR,
	SYS_OPENDIR,
	SYS_CLOSEDIR,
	SYS_CHDIR,
	SYS_RMDIR,
	SYS_READDIR,
	SYS_REWINDDIR,
	SYS_STAT,
	SYS_PS,
	SYS_EXECV,
	SYS_WAIT,
	SYS_EXIT,
	SYS_READRAW,
	SYS_PIPE,
	SYS_FD_REDIRECT,
	SYS_FREE_MEM,
	SYS_DISK_INFO,
	SYS_MOUNT
};


// user interface
extern uint32_t getpid(void);
// extern uint32_t write(char* str);
// extern uint32_t write_int(int num);

extern uint32_t write(int32_t fd,const void* buf,uint32_t count);
extern void* malloc(uint32_t size);
extern void free(void *ptr);
extern pid_t fork(void);
extern void clear(void);
extern int32_t read(int32_t fd,void* buf,uint32_t count);
extern void putchar(char char_ascii);
extern void clear(void);
extern char* getcwd(char* buf,uint32_t size);
extern int32_t open(char* pathname,uint8_t flag);
extern int32_t close(int32_t fd);
extern int32_t lseek(int32_t fd,int32_t offset,uint8_t whence);
extern int32_t unlink(const char* pathname);
extern int32_t mkdir(const char* pathname);
extern struct dir* opendir(const char* name);
extern int32_t closedir(struct dir* dir);
extern int32_t rmdir(const char* pathname);
extern struct dir_entry* readdir(struct dir* dir);
extern void rewinddir(struct dir* dir);
extern int32_t stat(const char* path,struct stat* buf);
extern int32_t chdir(const char* path);
extern void ps(void);
extern int32_t execv(const char* path,const char* argv[]);
extern pid_t wait(int32_t* status);
extern void exit(int32_t status);
extern void readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);
extern int32_t pipe(int32_t pipefd[2]);
extern void fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd);
extern void help(void);
extern void free_mem(void);
extern void disk_info(void);

#endif