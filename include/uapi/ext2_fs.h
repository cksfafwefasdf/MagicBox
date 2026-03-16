#ifndef __INCLUDE_UAPI_EXT2_FS_H
#define __INCLUDE_UAPI_EXT2_FS_H

#include <stdint.h>
#include <unistd.h>

#define EXT2_BLOCK_UNIT 1024
#define EXT2_BLOCK_SIZE(sb) ((sb)->s_block_size)
#define BLOCK_TO_SECTOR(sb, n) ((n) * ((sb)->s_block_size / SECTOR_SIZE))
#define EXT2_MAGIC_NUMBER 0xEF53

// 文件类型掩码 (i_mode Bit 12-15)
#define S_IFMT   0xF000

// 具体文件类型值, 由于 PIPE 没有磁盘镜像，所以这里没有PIPE
#define S_IFSOCK 0xC000  // 套接字 Socket 
#define S_IFLNK  0xA000  // 符号链接 Symbolic Link 
#define S_IFREG  0x8000  // 普通文件 Regular File 
#define S_IFBLK  0x6000  // 块设备 Block Device 
#define S_IFDIR  0x4000  // 目录 Directory 
#define S_IFCHR  0x2000  // 字符设备 Character Device 
#define S_IFIFO  0x1000  // 管道 FIFO 

// 判断文件类型
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

// 权限位，暂时用不到，先预留
#define S_IRWXU 00700 // 用户读写执行
#define S_IRUSR 00400 // 用户读 
#define S_IWUSR 00200 // 用户写 
#define S_IXUSR 00100 // 用户执行 

#define S_IRWXG 00070 // 组读写执行 
#define S_IRGRP 00040 // 组读 
#define S_IWGRP 00020 // 组写 
#define S_IXGRP 00010 // 组执行 

#define S_IRWXO 00007 // 其他人读写执行 
#define S_IROTH 00004 // 其他人读 
#define S_IWOTH 00002 // 其他人写 
#define S_IXOTH 00001 // 其他人执行 

// 需要注意的是，与我们之前的sifs不同
// 在 Ext2 中，无论块大小是多少，
// 前 1024 字节（扇区 0 和 1）永远是保留给引导加载程序（Boot Loader）的。
// 所以超级块永远从 1024 字节（扇区 2）开始。
// 关于GDT 的位置，
// 如果 block_size = 1024：Block 0 是保留区，Block 1 是超级块，Block 2 是 GDT。
// 如果 block_size > 1024：Block 0 包含了保留区和超级块，Block 1 是 GDT。
// 目前我们先固定参数 -b 1024，也就是说块大小默认为1024字节，那就锁定 Block 2（LBA 4）即可。

struct ext2_super_block {
    // [0-43字节] 核心计数与大小
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    int32_t  s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;

    // [44-55字节] 时间与挂载信息
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    int16_t  s_max_mnt_count;

    // [56-63字节] 魔数与状态
    uint16_t s_magic;      /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_pad;

    // [64-71字节] 检查信息
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;

    // [72-1023字节] 保留填充
    uint32_t s_reserved[238]; 
} __attribute__((packed));

// inode 的磁盘镜像
struct ext2_inode {
    uint16_t i_mode; // 文件类型和访问权限
    uint16_t i_uid; // 用户 ID
    uint32_t i_size; // 文件大小（字节）
    uint32_t i_atime; // 最后访问时间
    uint32_t i_ctime; // 创建时间
    uint32_t i_mtime; // 修改时间
    uint32_t i_dtime; // 删除时间 
    uint16_t i_gid; // 组 ID 
    uint16_t i_links_count; // 硬链接计数 
    uint32_t i_blocks; // 占用扇区数（Ext2 这里通常存 512B 扇区数）
    uint32_t i_flags; // 文件标志 
    uint32_t i_reserved1; // 保留 
    
    // 核心指针：12直接, 1一级间接, 1二级间接, 1三级间接
    uint32_t i_block[15];   
    
    uint32_t i_generation; // 文件版本 
    uint32_t i_file_acl; // 扩展属性 ACL */
    uint32_t i_dir_acl; // 目录 ACL (对文件来说是高位大小) 
    uint32_t i_faddr; // 碎片地址 
    uint8_t  i_frag; // 碎片编号 
    uint8_t  i_fsize; // 碎片大小 
    uint16_t i_pad1;
    uint32_t i_reserved2[2];
} __attribute__((packed)); 

struct ext2_dir_entry {
    uint32_t i_no; // Inode 编号
    uint16_t rec_len; // 目录项长度 
    uint8_t name_len; // 文件名长度 (0-255) 
    uint8_t  file_type; // 文件类型，使用 version 2 版本的目录项，他会存储文件类型，版本1不会
    char     name[255]; // 文件名 
} __attribute__((packed));

#endif