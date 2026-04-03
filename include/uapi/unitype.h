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

// mmap/munmap 相关协议常量，内核需要解析这些常量，用户需要传入这些常量
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_PRIVATE   0x02
#define MAP_ANON      0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void*)-1)

// mmap 函数的参数包
struct mmap_args {
    uint32_t addr;
    uint32_t len;
    uint32_t prot;
    uint32_t flags;
    int32_t fd;
    uint32_t offset;
};

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
	// 排他性创建，O_CREATE操作是文件不存在则创建，存在则打开
	// 加上O_EXCL则是只有在不存在时才创建，存在时报错不打开
	O_EXCL = 64, 
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
	// 文件名字符串，默认占一个固定长度来兼容sifs
	// 在 ext2 里面就使用 d_reclen 来定位，可以无视 MAX_FILE_NAME_LEN
    char d_name[MAX_FILE_NAME_LEN]; 
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

// struct stat{
// 	uint32_t st_ino;
// 	uint32_t st_size;
// 	enum file_types st_filetype;
// };

struct stat {
    uint64_t st_dev;      // 文件所在设备 ID
    uint32_t st_ino;      // Inode 编号
    uint32_t st_mode;     // 类型 + 权限 (例如 S_IFREG | 0644)
    uint32_t st_nlink;    // 连结数 (建议至少给个 1)
    uint32_t st_uid;      // 用户 ID (暂时填 0 也行)
    uint32_t st_gid;      // 组 ID
    uint64_t st_rdev;     // 若为特殊设备文件，其设备 ID
    int64_t  st_size;     // 文件大小 (用 int64 预防大文件)
    
    // 时间戳
    uint32_t st_atime;    // 最后访问
    uint32_t st_mtime;    // 最后修改
    uint32_t st_ctime;    // 最后状态改变

    uint32_t st_blksize; // 文件系统的块大小
    
    // 内部快捷访问字段
    enum file_types st_filetype; 
};

#endif
