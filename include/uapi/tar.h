#ifndef __LIB_COMMON_TAR_H
#define __LIB_COMMON_TAR_H
#include "stdint.h"

// POSIX ustar 标准头部 (512字节)
struct tar_header {
   char name[100];     // 文件名
   char mode[8];
   char uid[8];
   char gid[8];
   char size[12];      // 八进制字符串字节大小
   char mtime[12];
   char chksum[8];
   char typeflag;      // '0'文件, '2'符号链接, '5'目录
   char linkname[100];
   char magic[6];      // "ustar"
   char version[2];
   char uname[32];
   char gname[32];
   char devmajor[8];
   char devminor[8];
   char prefix[155];
   char padding[12];
} __attribute__((packed)); // 确保编译器不进行字节对齐填充

extern void untar_all(uint32_t base_lba);

#endif