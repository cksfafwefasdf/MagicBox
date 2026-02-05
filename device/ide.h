#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H
#include "stdint.h"
#include "dlist.h"
#include "bitmap.h"
#include "sync.h"
#include "stdbool.h"
#include "fs_types.h"


#define CHANNEL_NUM 2
#define MAX_DISK_NAME_LEN 8
#define DISK_NUM_IN_CHANNEL 2
#define PRIM_PARTS_NUM 4
#define LOGIC_PARTS_NUM 8
#define DEVICE_NUM_PER_CHANNEL 2
#define START_BYTE_PARTITION_TABLE 446
#define END_BYTE_PARTITION_TABLE 509
// 每轮读写操作连续读取的扇区数
// 设置 8 或 16。必须是 2 的幂，但不能超过硬盘支持的最大值（IDENTIFY Word 47）
#define SECTORS_PER_OP_BLOCK 16


// 对于第一块盘 sda：i_rdev 是 0x0300。
// sda1 就是 0x0300 + 1 = 0x0301。
// sda5 就是 0x0300 + 5 = 0x0305。
// 对于第二块盘 sdb：i_rdev 是 0x0310（即次设备号 16）。
// sdb1 就是 0x0310 + 1 = 0x0311。
// sdb5 就是 0x0310 + 5 = 0x0315。

// disk partition
struct partition{
	uint32_t start_lba;
	uint32_t sec_cnt;
	uint32_t i_rdev; // 逻辑设备号，用于在vfs中注册时使用
	struct disk* my_disk; // disk that the partition belongs
	struct dlist_elem part_tag; // be used in the queue
	char name[8]; // partition name
	struct super_block* sb; // super block in this partition
	struct bitmap block_bitmap; 
	struct bitmap inode_bitmap;
	struct dlist open_inodes; // inode openned by this partition
};

struct disk{
	char name[8]; // disk name
	struct ide_channel* my_channel; // ide channel that this disk belongs
	uint8_t dev_no; // master ide is 0, slave is 1
	struct partition prim_parts[4]; // the max number of primary partition is 4
	struct partition logic_parts[8]; // we only support 8 logic partition
	uint32_t i_rdev; // 逻辑设备号，用于在vfs中注册时使用
};

struct ide_channel{
	char name[8]; // name of ata channel
	uint16_t port_base; // the beginning number of the channel port
	uint8_t irq_no; // interrpt number used by this channel
	struct lock lock; // channel lock, only one thread can access this channel
	bool expecting_intr; // whether this channel is waiting the disk intr
	// when I/O operation occurs, proc can use this semaphore to block itself
	struct semaphore wait_disk; // is used to block and wakeup the driver prog
	struct disk devices[DEVICE_NUM_PER_CHANNEL]; // a channel has two disk, one for master channel, one for slave channel
};

extern void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_init(void);
extern void intr_handler_hd(uint8_t irq_no);
extern void sys_readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);
extern void sys_read_sectors(const char* hd_name,uint32_t lba, uint8_t* buf, uint32_t sec_cnt);
extern int32_t ide_dev_write(struct file* file, const void* buf, uint32_t count);
extern int32_t ide_dev_read(struct file* file, void* buf, uint32_t count);

extern struct ide_channel channels[2];
extern uint8_t channel_cnt;
extern struct dlist partition_list;
extern uint32_t* disk_size;
extern uint8_t disk_num;
struct file_operations ide_dev_fops;

#endif