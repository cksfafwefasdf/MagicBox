#ifndef __INCLUDE_UAPI_UNISTD_H
#define __INCLUDE_UAPI_UNISTD_H

#include <stdint.h>

#define MAX_PATH_LEN 512

#define PRINT_BUF_SIZE 1024 

#define MAX_FILE_NAME_LEN 64

#define UNUSED __attribute__((unused))

// waitpid 的参数
#define WNOHANG    1    // 0001
#define WUNTRACED  2    // 0010
#define WCONTINUED 4    // 0100

// 和 shell 有关的宏
#define CMD_LEN 128
#define MAX_ARG_NR 16
#define CMD_NUM 64

// 该类型定义和linux目录项中的完全一致，不需要特别转化
// 除了FT_PIPE，由于他不需要存储在硬盘上，因此linux没有定义它
enum file_types{
	FT_UNKNOWN, // unsupported type
	FT_REGULAR, // regular file
	FT_DIRECTORY, // directory file
	FT_CHAR_SPECIAL, // 字符设备
	FT_BLOCK_SPECIAL, // 块设备
	FT_FIFO, // 具名管道，不占磁盘数据块但inode要写回磁盘
	FT_SOCKET, // UNIX 套接字
	FT_SYMBOLIC_LINK, // 符号链接
	FT_PIPE, // 匿名管道，inode都不写回磁盘
};

enum oflags{ // operation flags
	O_RDONLY=1, // read only
	O_WRONLY=2, // write only
	O_RDWR=4, // read and write
	O_CREATE=8, // only create
	O_TRUNC = 16, // 从头开始读写文件
	O_APPEND = 32, // 从文件的末尾开始读写 
};

enum whence{
	SEEK_SET = 1,
	SEEK_CUR, // SEEK_CUR = 2
	SEEK_END // SEEK_END = 3
};

// 这是一个用户和内核之间的abi接口，这是VFS真正操纵的目录项结构体
// 我们现在取消了 dir 结构体，我们将 dir 结构体并入到 file 结构体中
// 将 dir 也看做一种文件，然后通过文件类型来分发操作
struct dirent {
    uint32_t d_ino; // Inode 编号
    uint32_t d_off; // 在目录文件中的偏移
    uint16_t d_reclen; // 当前 dirent 的长度
    uint8_t  d_type; // 文件类型
    char d_name[MAX_FILE_NAME_LEN]; // 文件名字符串
};

struct statfs {
    uint32_t f_type; // 文件系统类型（比如 SIFS 的魔数）
    uint32_t f_bsize; // 最优传输块大小（通常等于扇区大小，如 512 或 4096）
    uint32_t f_blocks; // 分区总共有多少个块（总容量）
    uint32_t f_bfree; // 剩余空闲块数
    uint32_t f_bavail; // 普通用户可用的空闲块（单用户系统中，通常 f_bavail = f_bfree）
    uint32_t f_files; // 分区总共有多少个 Inode 节点（总文件数上限）
    uint32_t f_ffree; // 剩余可用的 Inode 节点数
    uint32_t f_namelen; // 最大文件名长度
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