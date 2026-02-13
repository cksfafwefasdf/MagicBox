#ifndef __LIB_COMMON_UNISTD_H
#define __LIB_COMMON_UNISTD_H

#include "stdint.h"


#define MAX_PATH_LEN 512

#define PRINT_BUF_SIZE 1024 

#define MAX_FILE_NAME_LEN 16

#define UNUSED __attribute__((unused))

#define SECTOR_SIZE 512
#define BLOCK_SIZE SECTOR_SIZE

// waitpid 的参数
#define WNOHANG    1    // 0001
#define WUNTRACED  2    // 0010
#define WCONTINUED 4    // 0100

// 和 shell 有关的宏
#define CMD_LEN 128
#define MAX_ARG_NR 16
#define CMD_NUM 64

enum file_types{
	FT_UNKNOWN, // unsupported type
	FT_REGULAR, // regular file
	FT_DIRECTORY, // directory file
	FT_CHAR_SPECIAL, // 字符设备
	FT_BLOCK_SPECIAL, // 块设备
	FT_PIPE, // 匿名管道，inode都不写回磁盘
	FT_FIFO // 具名管道，不占磁盘数据块但inode要写回磁盘
};

enum oflags{ // operation flags
	O_RDONLY=1, // read only
	O_WRONLY=2, // write only
	O_RDWR=4, // read and write
	O_CREATE=8, // only create
};

enum whence{
	SEEK_SET = 1,
	SEEK_CUR, // SEEK_CUR = 2
	SEEK_END // SEEK_END = 3
};

// 不要把 dir 数据结构包含进来，因为dir数据结构中包含着一个inode类型的成员
// 将其包含进来会导致一连串的连锁反应
// 从而使得内核代码和用户代码严重耦合
struct dir_entry{
	char filename[MAX_FILE_NAME_LEN];
	uint32_t i_no;
	enum file_types f_type;
};

struct sigaction {
    void (*sa_handler)(int);
    uint32_t sa_mask;
    int32_t sa_flags;
    void (*sa_restorer)(void); // 信号执行完后的返回函数
};

enum std_fd{
	stdin_no,
	stdout_no,
	stderr_no
};

struct stat{
	uint32_t st_ino;
	uint32_t st_size;
	enum file_types st_filetype;
};

#endif