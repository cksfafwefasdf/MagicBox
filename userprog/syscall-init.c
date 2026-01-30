#include "syscall-init.h"
#include "stdint.h"
#include "thread.h"
#include "print.h"
#include "syscall.h"
#include "console.h"
#include "string.h"
#include "fs.h"
#include "fork.h"
#include "thread.h"
#include "exec.h"
#include "wait_exit.h"
#include "ide.h"
#include "pipe.h"

#define SYSCALL_NR 64
typedef void* syscall_func;
syscall_func syscall_table[SYSCALL_NR];

uint32_t sys_getpid(void){
	return get_running_task_struct()->pid;
}

// uint32_t sys_write(char* str){
// 	console_put_str(str);
// 	// how many bytes do sys_write write successfully
// 	return strlen(str);
// }

// uint32_t sys_write_int(int num){
// 	console_put_int_HAX(num);
// 	// how many bytes do sys_write write successfully
// 	return num;
// }

void syscall_init(void){
	put_str("syscall_init start\n");
	syscall_table[SYS_GETPID] = sys_getpid;
	syscall_table[SYS_WRITE] = sys_write;
	// syscall_table[SYS_WRITE_INT] = sys_write_int;
	syscall_table[SYS_MALLOC] = umalloc;
	syscall_table[SYS_FREE] = ufree;
	syscall_table[SYS_FORK] = sys_fork;
	syscall_table[SYS_READ] = sys_read;
	syscall_table[SYS_PUTCHAR] = console_put_char;
	syscall_table[SYS_CLEAR] = cls_screen;
	syscall_table[SYS_GETCWD] = sys_getcwd;
	syscall_table[SYS_OPEN] = sys_open;
	syscall_table[SYS_CLOSE] = sys_close;
	syscall_table[SYS_LSEEK] = sys_lseek;
	syscall_table[SYS_UNLINK] = sys_unlink;
	syscall_table[SYS_MKDIR] = sys_mkdir;
	syscall_table[SYS_OPENDIR] = sys_opendir;
	syscall_table[SYS_CLOSEDIR] = sys_closedir;
	syscall_table[SYS_CHDIR] = sys_chdir;
	syscall_table[SYS_RMDIR] = sys_rmdir;
	syscall_table[SYS_READDIR] = sys_readdir;
	syscall_table[SYS_REWINDDIR] = sys_rewinddir;
	syscall_table[SYS_STAT] = sys_stat;
	syscall_table[SYS_PS] = sys_ps;
	syscall_table[SYS_EXECV] = sys_execv;
	syscall_table[SYS_EXIT] = sys_exit;
	syscall_table[SYS_WAIT] = sys_wait;
	syscall_table[SYS_READRAW] = sys_readraw;
	syscall_table[SYS_PIPE] = sys_pipe;
	syscall_table[SYS_FD_REDIRECT] = sys_fd_redirect;
	syscall_table[SYS_FREE_MEM] = sys_free_mem;
	syscall_table[SYS_DISK_INFO] = sys_disk_info;
	syscall_table[SYS_MOUNT] = sys_mount;
	syscall_table[SYS_TEST] = sys_test;
	syscall_table[SYS_READ_SECTORS] = sys_read_sectors;
	
	put_str("syscall_init done\n");
}

