#ifndef __DEVICE_IDE_H
#define __DEVICE_IDE_H
#include "../lib/stdint.h"
#include "../lib/kernel/dlist.h"
#include "../lib/kernel/bitmap.h"
#include "../thread/sync.h"
#include "../lib/stdbool.h"

#define CHANNEL_NUM 2
#define MAX_DISK_NAME_LEN 8
#define DISK_NUM_IN_CHANNEL 2
#define SEC_SIZE 512
#define PRIM_PARTS_NUM 4
#define LOGIC_PARTS_NUM 8

// disk partition
struct partition{
	uint32_t start_lba;
	uint32_t sec_cnt;
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
};

struct ide_channel{
	char name[8]; // name of ata channel
	uint16_t port_base; // the beginning number of the channel port
	uint8_t irq_no; // interrpt number used by this channel
	struct lock lock; // channel lock, only one thread can access this channel
	bool expecting_intr; // whether this channel is waiting the disk intr
	// when I/O operation occurs, proc can use this semaphore to block itself
	struct semaphore wait_disk; // is used to block and wakeup the driver prog
	struct disk devices[2]; // a channel has two disk, one for master channel, one for slave channel
};

extern void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_init(void);
extern void intr_handler_hd(uint8_t irq_no);
extern void sys_readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);

extern struct ide_channel channels[2];
extern uint8_t channel_cnt;
extern struct dlist partition_list;

#endif