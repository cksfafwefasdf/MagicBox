#include <syscall.h>
#include <stdio.h>
#include <unitype.h>
#include <string.h>
#include <stdint.h>

// 该 syscall 文件主要是由用户程序包含的
// 而我们的所有用户程序都要链接运行时库 start.s
// 因此所有用户程序都能访问到 sig_restorer 这个函数
// 由于我们的init进程是一个用户进程，因此他只能调用syscall.h 中的系统调用，不能使用 sys_xxx函数
// 但是它又是随内核一起编译的，这就引发了一个问题，
// sig_restorer是随用户程序编译的，但是init这个随内核编译的用户程序并没有包含start.s，因此init它不知道sig_restorer的存在
// 因此我们将sig_restorer声明成弱引用，链接器在链接时，若没有找到该符号，那么就把他置为0，找到了就置为相应值

__attribute__((weak)) extern void sig_restorer(void);

// eax for int_no
// ebx for arg1
// ecx for arg2
// edx for arg3
// esi for arg4
// edi for arg5

#define _syscall0(SYSCALL_NUM) ({ \
	int retval; \
	asm volatile( \
	"int $0x77" \
	:"=a"(retval) \
	:"a"(SYSCALL_NUM) \
	:"memory" \
	); \
	retval; \
})

#define _syscall1(SYSCALL_NUM,ARG1) ({ \
	int retval; \
	asm volatile( \
		"int $0x77" \
		:"=a"(retval) \
		:"a"(SYSCALL_NUM),"b"(ARG1) \
		:"memory" \
	); \
	retval; \
})

#define _syscall2(SYSCALL_NUM,ARG1,ARG2) ({ \
	int retval; \
	asm volatile( \
		"int $0x77" \
		:"=a"(retval) \
		:"a"(SYSCALL_NUM),"b"(ARG1),"c"(ARG2) \
		:"memory" \
	); \
	retval; \
})


#define _syscall3(SYSCALL_NUM,ARG1,ARG2,ARG3) ({ \
	int retval; \
	asm volatile( \
		"int $0x77" \
		:"=a"(retval) \
		:"a"(SYSCALL_NUM),"b"(ARG1),"c"(ARG2),"d"(ARG3) \
		:"memory" \
	); \
	retval; \
})

#define _syscall4(SYSCALL_NUM,ARG1,ARG2,ARG3,ARG4) ({ \
	int retval; \
	asm volatile( \
		"int $0x77" \
		:"=a"(retval) \
		:"a"(SYSCALL_NUM),"b"(ARG1),"c"(ARG2),"d"(ARG3),"S"(ARG4) \
		:"memory" \
	); \
	retval; \
})

#define _syscall5(SYSCALL_NUM,ARG1,ARG2,ARG3,ARG4,ARG5) ({ \
	int retval; \
	asm volatile( \
		"int $0x77" \
		:"=a"(retval) \
		:"a"(SYSCALL_NUM),"b"(ARG1),"c"(ARG2),"d"(ARG3),"S"(ARG4),"D"(ARG5) \
		:"memory" \
	); \
	retval; \
})


uint32_t getpid(void){
	return _syscall0(SYS_GETPID);
}

uint32_t write(int32_t fd,const void* buf,uint32_t count){
	return _syscall3(SYS_WRITE,fd,buf,count);
}

pid_t fork(void){
	return _syscall0(SYS_FORK);
}

int32_t read(int32_t fd,void* buf,uint32_t count){
	return _syscall3(SYS_READ,fd,buf,count);
}

void putchar(char char_ascii){
	_syscall1(SYS_PUTCHAR,char_ascii);
}

void clear(void){
	_syscall0(SYS_CLEAR);
}

char* getcwd(char* buf,uint32_t size){
	return (char*)_syscall2(SYS_GETCWD,buf,size);
}

int32_t open(char* pathname,uint8_t flag){
	return _syscall2(SYS_OPEN,pathname,flag);
}

int32_t close(int32_t fd){
	return _syscall1(SYS_CLOSE,fd);
}

int32_t lseek(int32_t fd,int32_t offset,uint8_t whence){
	return _syscall3(SYS_LSEEK,fd,offset,whence);
}

int32_t unlink(const char* pathname){
	return _syscall1(SYS_UNLINK,pathname);
}

int32_t mkdir(const char* pathname){
	return _syscall1(SYS_MKDIR,pathname);
}

int32_t rmdir(const char* pathname){
	return _syscall1(SYS_RMDIR,pathname);
}

int32_t readdir(int32_t fd, struct dirent* de){
	return _syscall2(SYS_READDIR,fd,de);
}

void rewinddir(int32_t fd_dir){
	_syscall1(SYS_REWINDDIR,fd_dir);
}

int32_t stat(const char* path,struct stat* buf){
	return _syscall2(SYS_STAT,path,buf);
}

int32_t chdir(const char* path){
	return _syscall1(SYS_CHDIR,path);
}

void ps(void){
	_syscall0(SYS_PS);
}

int32_t execv(const char* path,const char* argv[]){
	return _syscall2(SYS_EXECV,path,argv);
}

void exit(int32_t status){
	_syscall1(SYS_EXIT,status);
}

pid_t wait(int32_t* status){
	return _syscall1(SYS_WAIT,status);
}

void readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size){
	_syscall4(SYS_READRAW,disk_name,lba,filename,file_size);
}

int32_t pipe(int32_t pipefd[2]){
	return _syscall1(SYS_PIPE,pipefd);
}

void free_mem(void){
	_syscall0(SYS_FREE_MEM);
}

void disk_info(void){
	_syscall0(SYS_DISK_INFO);
}

int32_t mount(char* dev_name, char* mount_path, char* type, unsigned long new_flags UNUSED, void * data UNUSED){
	return _syscall5(SYS_MOUNT,dev_name,mount_path,type,new_flags,data);
}

void test_func(){
	printf("test_func:::test_func start!\n");
	_syscall0(SYS_TEST);
	printf("test_func:::test_func done!\n");
}

void read_sectors(const char* hd_name,uint32_t lba, uint8_t* buf, uint32_t sec_cnt){
	_syscall4(SYS_READ_SECTORS,hd_name,lba,buf,sec_cnt);
}

int32_t dup2(uint32_t old_local_fd, uint32_t new_local_fd){
	return _syscall3(SYS_DUP2,old_local_fd,new_local_fd,new_local_fd);
}

int32_t mknod(const char* pathname, enum file_types type, uint32_t dev){
	return _syscall3(SYS_MKNOD,pathname,type,dev);
}

pid_t setpgid(pid_t pid, pid_t pgid){
	return _syscall2(SYS_SETPGID,pid,pgid);
}
pid_t getpgid(pid_t pid){
	return _syscall1(SYS_GETPGID,pid);
}

int32_t ioctl(int fd, uint32_t cmd, uint32_t arg){
	return _syscall3(SYS_IOCTL,fd,cmd,arg);
}

void* signal(int sig, void* handler){
	struct sigaction action;
	struct sigaction old_action;
	// 一定要记得初始化，防止上一个结束的程序和当前程序的栈重合
	// 然后上一个程序的脏数据污染了我们的sigaction结构体
	// 如果脏数据正好位于我们的sa_restorer字段，那么sa_restorer就不为NULL了
	// sigaction 的 sa_restorer 默认赋值逻辑就无效了
	memset(&action, 0, sizeof(struct sigaction));
	action.sa_handler = handler;
	action.sa_mask = 0;
	action.sa_flags = 0;
	action.sa_restorer = sig_restorer;

	sigaction(sig,&action,&old_action);
	return old_action.sa_handler;
}

int pause(void){
	return _syscall0(SYS_PAUSE);
}

int32_t alarm(uint32_t seconds){
	return _syscall1(SYS_ALARM,seconds);
}

int sigaction(int sig, struct sigaction* act, struct sigaction* oact){
	if (act && act->sa_restorer == NULL) {
        act->sa_restorer = sig_restorer; // 默认为 start.S 里的跳板
    }
    return _syscall3(SYS_SIGACTION, sig, act, oact);
}

pid_t waitpid(pid_t pid, int32_t* status, int32_t options){
    return _syscall3(SYS_WAITPID, pid, status, options);
}

int kill(pid_t pid, int sig){
	return _syscall2(SYS_KILL, pid, sig);
}

int sigpending(uint32_t* set){
	return _syscall1(SYS_SIGPENDING, set);
}

int sigprocmask(int how, const uint32_t* set, uint32_t* oldset){
	return _syscall3(SYS_SIGPROCMASK, how, set, oldset);
}

int32_t mkfifo(const char* pathname){
	return _syscall1(SYS_MKFIFO, pathname);
}

int32_t umount(const char* _mount_path){
	return _syscall1(SYS_UMOUNT,_mount_path);
}

int32_t rename(const char* _old_path, const char* _new_path){
	return _syscall2(SYS_RENAME,_old_path,_new_path);
}	

int32_t statfs(const char* path, struct statfs* buf){
	return _syscall2(SYS_STATFS,path,buf);
}

void* brk(void* addr){
	return (void*)_syscall1(SYS_BRK, addr);
}

// 我们目前的系统调用中，最多只支持5个参数
// 但是比较标准的实现中，mmap需要6个参数
// 因此我们可以将这些参数进行打包
// 然后直接将整个数据包作为一个单独的参数，调用_syscall1来传入sys_mmap中
// 这样改动比较小
void* mmap(void* addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t offset){
	struct mmap_args args;
	args.addr = (uint32_t)addr;
	args.len = len;
	args.prot = prot;
	args.flags = flags;
	args.fd = fd;
	args.offset = offset;
	return (void*)_syscall1(SYS_MMAP, &args);
}

int32_t munmap(void* addr, uint32_t len){
	return _syscall2(SYS_MUNMAP, addr, len);
}

uint32_t time(void){
	return _syscall0(SYS_TIME);
}

int32_t symlink(const char* target, const char* linkpath){
	return _syscall2(SYS_SYMLINK,target,linkpath);
}

int32_t execve(const char* path, const char* argv[], const char* envp[]){
	return _syscall3(SYS_EXECVE,path,argv,envp);
}

int32_t lstat(const char* _pathname, struct stat* buf){
	return _syscall2(SYS_LSTAT,_pathname,buf);
}

int32_t msleep(uint32_t mil_seconds){
	return _syscall1(SYS_MILSLEEP,mil_seconds);
}

int32_t fcntl(int32_t fd, uint32_t cmd, uint32_t arg){
	return _syscall3(SYS_FCNTL, fd, cmd, arg);
}

int32_t readlink(const char* _path, char* buf, int32_t bufsize){
	return _syscall3(SYS_READLINK, _path, buf, bufsize);
}

int32_t poll(struct pollfd* fds, uint32_t nfds, int32_t timeout_ms){
	return _syscall3(SYS_POLL, fds, nfds, timeout_ms);
}

int32_t rt_sigaction(int sig, const struct sigaction* act, struct sigaction* oact, uint32_t sigsetsize){
	return _syscall4(SYS_RT_SIGACTION, sig, act, oact, sigsetsize);
}

pid_t getppid(){
	return _syscall0(SYS_GETPPID);
}

int32_t truncate(const char* path, int32_t length) {
	return _syscall2(SYS_TRUNCATE, path, length);
}

int32_t ftruncate(int32_t fd, int32_t length) {
	return _syscall2(SYS_FTRUNCATE, fd, length);
}

int32_t link(const char* _oldpath, const char* _newpath){
	return _syscall2(SYS_LINK, _oldpath, _newpath);
}

void sync(){
	_syscall0(SYS_SYNC);
}

int32_t swapon(const char* _pathname){
	return _syscall1(SYS_SWAPON,_pathname);
}

int32_t swapoff(const char* _pathname){
	return _syscall1(SYS_SWAPOFF,_pathname);
}

int32_t mprotect(uint32_t addr, uint32_t len, uint32_t new_flags){
	return _syscall3(SYS_MPROTECT, addr, len, new_flags);
}