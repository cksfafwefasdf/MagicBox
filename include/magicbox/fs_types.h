#ifndef __INCLUDE_MAGICBOX_FS_TYPES_H
#define __INCLUDE_MAGICBOX_FS_TYPES_H
#include "stdint.h"
#include "unistd.h"
#include "sifs_inode.h"
#include "sifs_sb.h"
#include "pipe.h"

/*
	虚拟文件系统层的头文件
	主要用于存储和具体文件系统无关的
	VFS 数据结构
*/

// the max number of the file in each partitions is 4096
#define MAX_FILES_PER_PART 4096
// each sector has 512 B, which equals to 512*8=4096 b
#define BITS_PER_SECTOR 4096 

#define ADDR_BYTES_32BIT 4 // size of the 32 bits address pointor

// 匿名 inode 的 i_no
#define ANONY_I_NO 0xffffffff

enum bitmap_type{
	INODE_BITMAP,
	BLOCK_BITMAP
};

// 内存inode
// VFS 直接操作的inode 对象
struct inode{
	enum file_types i_type;
	// 在目录文件中 i_size 标记的是该目录文件目前所达到的最大逻辑偏移量。
	// 对于目录文件而言，这个值只增不减，只会越变越大，但是对于普通文件而言他是会减小的！
	// 目录文件i_size只增不减的目的是为了避免目录“空洞”问题，比如 1 2 3 中，删除了 2，这样就剩下 1 X 3，中间的X是空洞
	// 如果我们将 i_size 减小的话，那么我们在 readdir 当中的逻辑就不好写了，因为此时我们的 i_size 的大小只是两个目录项，这样的话 readdir 读两次就停止了
	// 根本识别不到第三个 3 了！因此为了便于实现 readdir ，我们需要让探测的序列是 “连续” 的，因此我们删除文件时，就只是回收这部分空间，但是 i_size 并不减小
	// 这么干看起来很奇怪，但是其实linux的ext4文件系统也是这么实现的！可以做实验验证
	// 这个只增不减的原理其实有点类似于用线性探测法来解决哈希冲突，在线性哈希表中，如果删除元素，也是只是将文件标记为可覆盖，不会真将其删除
	// 否则会影响在这个文件之后的其他文件的查找，这里的道理也类似
	uint32_t i_size;
	uint32_t i_rdev; // 这个 inode 表示哪一个设备（针对设备inode使用）
	uint32_t i_no;
	uint32_t i_dev; // 这个 inode 存在哪个持久化设备上
	// 有多少个全局打开文件表项指向这个inode
	uint32_t i_open_cnts;
	// write operation will cause concurrent safty problem
	// so make write_deny true, before write the file. 
	bool write_deny;
	// this tag is used for the 'already opened inode queue'
	// to prevent redundant reads of inodes from the disk.

	struct dlist_elem lru_tag; // 哈希表节点：用于根据 (i_dev, i_no) 快速找到 inode
    struct dlist_elem hash_tag;  // LRU节点：用于当缓冲区满时，决定踢掉哪个 inode
	union{
		struct sifs_inode_info sifs_i;
		struct pipe_inode_info pipe_i;
	};
};

struct file{
	uint32_t fd_pos;
	uint32_t fd_flag;
	struct inode* fd_inode;
	// enum file_types f_type;
	// f_count 用于表示有多少局部FD指向此全局表中的 file，主要用于处理fork和dup2
	// inode 的 i_open_cnts 表示有多少个全局 file结构指向同一个inode
	// 通过多设置一个 f_count，将inode的生命周期管理和file的生命周期管理分开了
	uint32_t f_count;     
	struct file_operations* fops; // 指向该文件的具体操作集
};

struct path_search_record{
	char searched_path[MAX_PATH_LEN];
	struct inode* parent_inode; // 父目录的inode
	enum file_types file_type;
    uint32_t i_dev; // 所在设备号
};

// 用于 vfs，抽象文件操作
struct file_operations {
    int32_t (*read) (struct file* file, void* buf, uint32_t count);
    int32_t (*write)(struct file* file, void* buf, uint32_t count);
    int32_t (*open) (struct inode* inode, struct file* file);
    int32_t (*release)(struct file* file);
    // 预留 ioctl 来设置 TTY 属性 
    int32_t (*ioctl)(struct file* file, uint32_t cmd, unsigned long arg);
};


struct super_block {
    // VFS 通用字段
    uint32_t s_dev; // 超级块所存储在的逻辑设备号
    uint32_t s_block_size; 
    uint32_t s_root_ino; // 该超级块的根目录的 inode 的编号
	uint32_t s_magic;
    struct inode* s_root_inode; // 指向该超级块的根目录的 inode
    
    // VFS 对不同文件系统打的补丁
    union {
        struct sifs_sb_info sifs_info; // 此时这里就包含了运行时的状态
        // struct ext2_sb_info ext2_info;
    };
};

// 用来定义超级块的填充操作
// 主要用于填充 info 结构体
struct file_system_type {
	struct super_block *(*read_super) (struct super_block *, void *, int);
	char *name;
	int requires_dev;
};
#endif