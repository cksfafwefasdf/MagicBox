#ifndef __INCLUDE_UAPI_EXT2_FS_H
#define __INCLUDE_UAPI_EXT2_FS_H

#include <stdint.h>
#include <unistd.h>

#define EXT2_BLOCK_UNIT 1024
#define EXT2_BLOCK_SIZE(sb) ((sb)->s_block_size)
#define BLOCK_TO_SECTOR(sb, n) ((n) * ((sb)->s_block_size / SECTOR_SIZE))
#define EXT2_MAGIC_NUMBER 0xEF53
#define EXT2_MAX_FILE_NAME_LEN 255
// 此宏可以确保每一个目录项的起始地址都是 4 字节对齐的
// 并且为目录项结构体预留足够的空间。
// 8 是 struct ext2_dir_entry 的大小
// name_len，顾名思义，是元数据后的名称部分
// +3 然后 &~3用于四字节对齐
// 假设 name_len 加上头部后不是 4 的倍数，加上 3 可以确保它跨入下一个 4 字节的边界
// & ~3 就是将低3位1置为0，自然就是对齐操作
#define EXT2_DIR_REC_LEN(name_len) (((8 + (name_len)) + 3) & ~3)

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

    // 为了兼容 version 1，添加这些字段

    // 插入这两个字段来填补 4 字节的坑，一遍将后面的字段挤到磁盘正确的位置上
    uint16_t s_creator_os;     // 填 0 (Linux)
    uint16_t s_minor_rev_level;// 填 0

    uint32_t s_rev_level; // 填 1，用于指明ext2的版本号为1
    uint16_t s_def_resuid; // 80: Default uid for reserved blocks
    uint16_t s_def_resgid; // 82: Default gid for reserved blocks 
    
    uint32_t s_first_ino; // 顾名思义，第一个inode号，默认填 11
    uint16_t s_inode_size; // 填 128
    uint16_t s_block_group_nr;  // 偏移 84: 当前超级块所在的块组号，填 0
    uint32_t s_feature_compat;  // 偏移 88: 填 0
    // 这是不兼容特性位
    // 我们填 0x02 (FILETYPE)，这样目录项会存 file_type
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;// 偏移 96: 填 0
    
    uint8_t  s_uuid[16];        // 偏移 100
    char     s_volume_name[16]; // 偏移 116
    char     s_last_mounted[64]; // 偏移 132
    uint32_t s_algo_bitmap;     // 偏移 196

    // 填充剩余空间，使结构体达到 1024 字节
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_padding_1;
    uint32_t s_reserved[204];
} __attribute__((packed));

// inode 的磁盘镜像，不出意外的话，它的大小应该是128字节
struct ext2_inode {
    uint16_t i_mode; // 文件类型和访问权限
    uint16_t i_uid; // 用户 ID
    uint32_t i_size; // 文件大小（字节）
    uint32_t i_atime; // 最后访问时间
    uint32_t i_ctime; // 创建时间
    uint32_t i_mtime; // 修改时间
    uint32_t i_dtime; // 删除时间 
    uint16_t i_gid; // 组 ID 
    // 硬链接计数，硬链接本质就是有多少个不同的目录项指向同一个inode
    // 初值通常为2，自己目录下的 . 和父目录下的相应条目，这两个条目记了两次 
    uint16_t i_links_count; 
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
    uint16_t rec_len; // 目录项长度, 它是 ext2_dir_entry 这个结构体以及其后存储的 name 的总长度
    uint8_t name_len; // 文件名长度 (0-255) 
    uint8_t  file_type; // 文件类型，使用 version 2 版本的目录项，他会存储文件类型，版本1不会
    // 这个 name 字段不会被分配地址空间
    // 它的作用就只是一个标记符，用于标记 ext2_dir_entry 结构体的末尾地址
    // ext2_dir_entry 作用就有点像 arena 里面的描述符，真正的文件名数据不是存在ext2_dir_entry中的
    // ext2_dir_entry 只存储元信息，真正的 name 信息是紧跟着 ext2_dir_entry 结构体存在其后面的
    // 这样就能实现真正意义上的变长目录项
    char name[0]; // 文件名 
} __attribute__((packed));

#endif