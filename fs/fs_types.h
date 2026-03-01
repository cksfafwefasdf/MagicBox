#ifndef __FS_FS_TYPES_H
#define __FS_FS_TYPES_H
#include "stdint.h"
#include "unistd.h"
#include "dlist.h"
#include "sifs_inode.h"
#include "pipe.h"

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

// 我们现在代码中的inode基本上都可以直接无脑创建成 inode 以防出错
// 只在同步回磁盘时写成 d_inode 来瘦身
// 内存inode
struct inode{
	enum file_types i_type;
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
	struct dlist_elem inode_tag;
	union{
		struct sifs_inode_info sifs_i;
		struct pipe_inode_info pipe_i;
	};
};

struct dir{
	struct inode* inode;
	// Used to record the offset of the 'cursor' 
	// in the directory while traversing the directory.
	uint32_t dir_pos;
	uint8_t dir_buf[BLOCK_SIZE];
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
	struct dir* parent_dir;
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

struct super_block{
	uint32_t magic; // the type of the file system
	uint32_t sec_cnt; // the number of the sector in this partition
	uint32_t inode_cnt; // the number of the inode in this partition 
	uint32_t part_lba_base; // the start LBA of this partition
	
	uint32_t block_bitmap_lba;
	uint32_t block_bitmap_sects;
	
	uint32_t inode_bitmap_lba;
	uint32_t inode_bitmap_sects;

	uint32_t inode_table_lba;
	uint32_t inode_table_sects;

	uint32_t data_start_lba; // the start of the data setion in this partition
	uint32_t root_inode_no; // the inode number of the root dict
	uint32_t dir_entry_size; // size of each dict entry

	// make the size of the superblock be 512B (1 sector)
	uint8_t pad[460];

}__attribute__((packed));

#endif