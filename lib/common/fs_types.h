#ifndef __LIB_COMMON_FS_TYPES_H
#define __LIB_COMMON_FS_TYPES_H

#include "stdint.h"


#define MAX_PATH_LEN 512

#define PRINT_BUF_SIZE 1024 

#define MAX_FILE_NAME_LEN 16

#define UNUSED __attribute__((unused))

#define SECTOR_SIZE 512
#define BLOCK_SIZE SECTOR_SIZE


enum file_types{
	FT_UNKNOWN, // unsupported type
	FT_REGULAR, // regular file
	FT_DIRECTORY // directory file
};

enum oflags{ // operation flags
	O_RDONLY, // read only
	O_WRONLY, // write only
	O_RDWR, // read and write
	O_CREATE=4 // only create 
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