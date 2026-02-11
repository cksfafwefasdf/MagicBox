#ifndef __FS_FS_TYPES_H
#define __FS_FS_TYPES_H
#include "stdint.h"
#include "unistd.h"
#include "dlist.h"


// the max number of the file in each partitions is 4096
#define MAX_FILES_PER_PART 4096
// each sector has 512 B, which equals to 512*8=4096 b
#define BITS_PER_SECTOR 4096 

#define ADDR_BYTES_32BIT 4 // size of the 32 bits address pointor

#define FS_MAGIC_NUMBER 0x20030000 // magic number for this file system

#define DIRECT_INDEX_BLOCK 12
#define FIRST_LEVEL_INDEX_BLOCK 1

#define BLOCK_PTR_NUMBER DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK

#define TOTAL_BLOCK_COUNT (DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK*(BLOCK_SIZE/ADDR_BYTES_32BIT))

// 匿名 inode 的 i_no
#define ANONY_I_NO 0xffffffff

enum bitmap_type{
	INODE_BITMAP,
	BLOCK_BITMAP
};

// 磁盘inode
// 使用组合式的逻辑来连接 m_inode 和 d_inode
// 否则我们每次在d_inode中加一个属性都要同步到m_inode中，比较麻烦！
struct d_inode{
	// when inode points to file, i_size is the size of the file
	// when inode points to dict file, i_size is the size of the sum of the dict entry
	uint32_t i_size;
	// 0~11 are direct pointors, 12 is the level-one indirect pointor 
	// 联合体的大小由最大的属性大小决定
	// 像此处，就是由i_sectors[BLOCK_PTR_NUMBER]决定的
	// i_rdev 只用到前面的4字节，后面的字节全部空闲
	// 由于 设备 dev 所对应的 inode 并不需要存储索引块
	// 因此我们设置一个联合体，当为设备时存储目标设备号
	union {
		uint32_t i_sectors[BLOCK_PTR_NUMBER]; // 普通文件：存磁盘块索引
        uint32_t i_rdev; // 设备文件：存目标设备号
		uint32_t i_pipe_ptr; // 管道文件：指向 struct pipe 的内存地址
    };
	enum file_types i_type;
};

// 我们现在代码中的inode基本上都可以直接无脑创建成 m_inode 以防出错
// 只在同步回磁盘时写成 d_inode 来瘦身
// 内存inode
struct m_inode{
	// 磁盘inode一定要作为第一个成员！
	// 以便于使用强转来快速截断m_inode，得到d_inode，
	// 如果不是第一个成员强转显然会出错
	// 但是其实不用强转也没事，有组合关系的话直接 m_inode.di 就行了，还更安全
	// 但是我们最好还是保留一下这个特性
	struct d_inode di;
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
};

struct dir{
	struct m_inode* inode;
	// Used to record the offset of the 'cursor' 
	// in the directory while traversing the directory.
	uint32_t dir_pos;
	uint8_t dir_buf[BLOCK_SIZE];
};

struct file{
	uint32_t fd_pos;
	uint32_t fd_flag;
	struct m_inode* fd_inode;
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
    int32_t (*write)(struct file* file, const void* buf, uint32_t count);
    int32_t (*open) (struct m_inode* inode, struct file* file);
    int32_t (*close)(struct file* file);
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