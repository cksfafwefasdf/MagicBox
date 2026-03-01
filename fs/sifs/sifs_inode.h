#ifndef __FS_SIFS_INODE_H
#define __FS_SIFS_INODE_H
#include "stdint.h"
#include "stdbool.h"
#include "unistd.h"

#define DIRECT_INDEX_BLOCK 12
#define FIRST_LEVEL_INDEX_BLOCK 1

#define BLOCK_PTR_NUMBER DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK

#define TOTAL_BLOCK_COUNT (DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK*(BLOCK_SIZE/ADDR_BYTES_32BIT))

struct partition;
struct inode;

// 针对不同的文件系统，在VFS数据结构inode中所打的补丁
struct sifs_inode_info{
	// 0~11 are direct pointors, 12 is the level-one indirect pointor 
	// 联合体的大小由最大的属性大小决定
	// 像此处，就是由i_sectors[BLOCK_PTR_NUMBER]决定的
	// i_rdev 只用到前面的4字节，后面的字节全部空闲
	// 由于 设备 dev 所对应的 inode 并不需要存储索引块
	// 因此我们设置一个联合体，当为设备时存储目标设备号
	union {
		uint32_t i_sectors[BLOCK_PTR_NUMBER]; // 普通文件：存磁盘块索引
        uint32_t i_rdev; // 设备文件：存目标设备号
    };
};

// 针对不同文件系统，存储在磁盘上的inode实体
struct sifs_inode{
	enum file_types i_type;
	// when inode points to file, i_size is the size of the file
	// when inode points to dict file, i_size is the size of the sum of the dict entry
	uint32_t i_size;
	struct sifs_inode_info sii;
};

extern void inode_init(struct partition* part, uint32_t inode_no,struct inode* new_inode,enum file_types ft);
extern void inode_close(struct inode* inode);
extern struct inode* inode_open(struct partition* part,uint32_t inode_no);
extern void inode_sync(struct partition* part,struct inode* inode,void* io_buf);
extern void inode_release(struct partition* part,uint32_t inode_no);
extern void inode_delete(struct partition* part,uint32_t inode_no,void* io_buf);
extern struct inode* make_anonymous_inode(void);
extern int32_t inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count);

#endif