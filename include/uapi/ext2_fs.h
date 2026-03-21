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

// 直接使用 linux 里面的定义
struct ext2_super_block {
	uint32_t	s_inodes_count;		/* Inodes count */
	uint32_t	s_blocks_count;		/* Blocks count */
	uint32_t	s_r_blocks_count;	/* Reserved blocks count */
	uint32_t	s_free_blocks_count;	/* Free blocks count */
	uint32_t	s_free_inodes_count;	/* Free inodes count */
	uint32_t	s_first_data_block;	/* First Data Block */
	uint32_t	s_log_block_size;	/* Block size */
	uint32_t	s_log_frag_size;	/* Fragment size */
	uint32_t	s_blocks_per_group;	/* # Blocks per group */
	uint32_t	s_frags_per_group;	/* # Fragments per group */
	uint32_t	s_inodes_per_group;	/* # Inodes per group */
	uint32_t	s_mtime;		/* Mount time */
	uint32_t	s_wtime;		/* Write time */
	uint16_t	s_mnt_count;		/* Mount count */
	uint16_t	s_max_mnt_count;	/* Maximal mount count */
	uint16_t	s_magic;		/* Magic signature */
	uint16_t	s_state;		/* File system state */
	uint16_t	s_errors;		/* Behaviour when detecting errors */
	uint16_t	s_minor_rev_level; 	/* minor revision level */
	uint32_t	s_lastcheck;		/* time of last check */
	uint32_t	s_checkinterval;	/* max. time between checks */
	uint32_t	s_creator_os;		/* OS */
	uint32_t	s_rev_level;		/* Revision level */
	uint16_t	s_def_resuid;		/* Default uid for reserved blocks */
	uint16_t	s_def_resgid;		/* Default gid for reserved blocks */
	/*
	 * These fields are for EXT2_DYNAMIC_REV superblocks only.
	 *
	 * Note: the difference between the compatible feature set and
	 * the incompatible feature set is that if there is a bit set
	 * in the incompatible feature set that the kernel doesn't
	 * know about, it should refuse to mount the filesystem.
	 * 
	 * e2fsck's requirements are more strict; if it doesn't know
	 * about a feature in either the compatible or incompatible
	 * feature set, it must abort and not try to meddle with
	 * things it doesn't understand...
	 */
	uint32_t	s_first_ino; 		/* First non-reserved inode */
	uint16_t   s_inode_size; 		/* size of inode structure */
	uint16_t	s_block_group_nr; 	/* block group # of this superblock */
	uint32_t	s_feature_compat; 	/* compatible feature set */
	uint32_t	s_feature_incompat; 	/* incompatible feature set */
	uint32_t	s_feature_ro_compat; 	/* readonly-compatible feature set */
	uint8_t	s_uuid[16];		/* 128-bit uuid for volume */
	char	s_volume_name[16]; 	/* volume name */
	char	s_last_mounted[64]; 	/* directory where last mounted */
	uint32_t	s_algorithm_usage_bitmap; /* For compression */
	/*
	 * Performance hints.  Directory preallocation should only
	 * happen if the EXT2_COMPAT_PREALLOC flag is on.
	 */
	uint8_t	s_prealloc_blocks;	/* Nr of blocks to try to preallocate*/
	uint8_t	s_prealloc_dir_blocks;	/* Nr to preallocate for dirs */
	uint16_t	s_padding1;
	/*
	 * Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
	 */
	uint8_t	s_journal_uuid[16];	/* uuid of journal superblock */
	uint32_t	s_journal_inum;		/* inode number of journal file */
	uint32_t	s_journal_dev;		/* device number of journal file */
	uint32_t	s_last_orphan;		/* start of list of inodes to delete */
	uint32_t	s_hash_seed[4];		/* HTREE hash seed */
	uint8_t	s_def_hash_version;	/* Default hash version to use */
	uint8_t	s_reserved_char_pad;
	uint16_t	s_reserved_word_pad;
	uint32_t	s_default_mount_opts;
 	uint32_t	s_first_meta_bg; 	/* First metablock block group */
	uint32_t	s_reserved[190];	/* Padding to the end of the block */
} __attribute__((packed)) ;

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