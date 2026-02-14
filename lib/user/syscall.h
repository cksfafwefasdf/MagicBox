#ifndef __LIB_USER_SYSCALL_H
#define __LIB_USER_SYSCALL_H
#include "stdint.h"
#include "unistd.h"

// syscall.h includes interface for user

#define	SYS_GETPID 0
#define	SYS_WRITE 1
#define	SYS_MALLOC 2
#define	SYS_FREE 3
#define	SYS_FORK 4
#define	SYS_READ 5
#define	SYS_PUTCHAR 6
#define	SYS_CLEAR 7
#define	SYS_GETCWD 8
#define	SYS_OPEN 9
#define	SYS_CLOSE 10
#define	SYS_LSEEK 11
#define	SYS_UNLINK 12
#define	SYS_MKDIR 13
#define	SYS_OPENDIR 14
#define	SYS_CLOSEDIR 15
#define	SYS_CHDIR 16
#define	SYS_RMDIR 17
#define	SYS_READDIR 18
#define	SYS_REWINDDIR 19
#define	SYS_STAT 20
#define	SYS_PS 21
#define	SYS_EXECV 22
#define	SYS_WAIT 23
#define	SYS_EXIT 24
#define	SYS_READRAW 25
#define	SYS_PIPE 26
#define	SYS_FREE_MEM 27
#define	SYS_DISK_INFO 28
#define	SYS_MOUNT 29
#define	SYS_TEST 30
#define	SYS_READ_SECTORS 31
#define	SYS_MKNOD 32 
#define	SYS_DUP2 33
#define	SYS_SETPGID 34
#define	SYS_GETPGID 35
#define	SYS_IOCTL 36
#define	SYS_SIGNAL 37
#define	SYS_ALARM 38
#define	SYS_PAUSE 39
#define	SYS_SIGRETURN 40 // 这是个协议系统调用，不开放给用户，但是为了系统调用的规范性，还是在此注册一下
#define	SYS_SIGACTION 41
#define	SYS_WAITPID 42
#define	SYS_KILL 43
#define	SYS_SIGPENDING 44
#define	SYS_SIGPROCMASK 45
#define SYS_MKFIFO 46

// user interface
extern uint32_t getpid(void);
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
extern int32_t opendir(const char* name);
extern int32_t closedir(int32_t fd_dir);
extern int32_t readdir(int32_t fd, struct dir_entry* de);
extern void rewinddir(int32_t fd_dir);
extern int32_t rmdir(const char* pathname);
extern int32_t stat(const char* path,struct stat* buf);
extern int32_t chdir(const char* path);
extern void ps(void);
extern int32_t execv(const char* path,const char* argv[]);
extern pid_t wait(int32_t* status);
extern void exit(int32_t status);
extern void readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);
extern int32_t pipe(int32_t pipefd[2]);
extern void help(void);
extern void free_mem(void);
extern void disk_info(void);
extern void test_func(void);
extern void read_sectors(const char* hd_name,uint32_t lba, uint8_t* buf, uint32_t sec_cnt);
extern void mount(const char* part_name);
extern int32_t mknod(const char* pathname, enum file_types type, uint32_t dev);
extern int32_t dup2(uint32_t old_local_fd, uint32_t new_local_fd);
extern pid_t setpgid(pid_t pid, pid_t pgid);
extern pid_t getpgid(pid_t pid);
extern int32_t ioctl(int fd, uint32_t cmd, uint32_t arg);
extern void* signal(int sig, void* handler);
extern uint32_t alarm(uint32_t seconds);
extern int pause(void);
extern int sigaction(int sig, struct sigaction* act, struct sigaction* oact);
extern pid_t waitpid(pid_t pid, int32_t* status, int32_t options);
extern int kill(pid_t pid, int sig);
extern int sigpending(uint32_t* set);
extern int sigprocmask(int how, const uint32_t* set, uint32_t* oldset);
extern int32_t mkfifo(const char* pathname);

#endif