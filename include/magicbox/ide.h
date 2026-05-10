#ifndef __INCLUDE_MAGICBOX_IDE_H
#define __INCLUDE_MAGICBOX_IDE_H
#include <stdint.h>
#include <dlist.h>
#include <bitmap.h>
#include <sync.h>
#include <stdbool.h>
#include <fs_types.h>
#include <ide_buffer.h>
#include <ide_dma.h>

#define reg_data(channel) (channel->port_base+0)
#define reg_error(channel) (channel->port_base+1)
#define reg_feature(channel) (reg_error(channel))
#define reg_sect_cnt(channel) (channel->port_base+2)
#define reg_lba_l(channel) (channel->port_base+3)
#define reg_lba_m(channel) (channel->port_base+4)
#define reg_lba_h(channel) (channel->port_base+5)
#define reg_dev(channel) (channel->port_base+6)
#define reg_status(channel) (channel->port_base+7)
#define reg_cmd(channel) (reg_status(channel))
#define reg_alt_status(channel) (channel->port_base+0x206)
#define reg_ctl(channel) (reg_alt_status(channel))

// important bit in status port or device port
#define BIT_ALT_STAT_BSY 0x80
#define BIT_ALT_STAT_DRDY 0x40
#define BIF_ALT_STAT_DRQ 0x8
#define BIT_DEV_MBS 0xa0 // 10100000, these bits are always set to 1
#define BIT_DEV_LBA 0x40 // use LBA instead of CHS
#define BIT_DEV_DEV 0x10 // 0 is master 1 is slave

// commands to control the disk
// these commands should be written in the reg_cmd port 
#define CMD_IDENTIFY 0xec
#define CMD_READ_SECTOR 0x20
#define CMD_WRITE_SECTOR 0x30

#define CMD_DMA_READ 0xC8
#define CMD_DMA_WRITE 0xCA

#define CMD_SET_MULTIPLE 0xC6 // 设置每次读取的块大小
#define CMD_READ_MULTIPLE 0xC4 // 多扇区读取指令
#define CMD_WRITE_MULTIPLE 0xC5  // 多扇区写入指令

#define max_lba ((80*1024*1024/512)-1) // only surport 80MB disk

// the number of the disk is stored in this addr by BIOS 
#define BIOS_DISK_NUM_ADDR 0x475
#define DISK_PARAM_ADDR 0x501

#define BUSY_WAIT_TIME_LIMIT 30*1000

#define CHANNEL_NUM 2
#define MAX_DISK_NAME_LEN 8
#define DISK_NUM_IN_CHANNEL 2
#define PRIM_PARTS_NUM 4
#define LOGIC_PARTS_NUM 8
#define DEVICE_NUM_PER_CHANNEL 2
#define START_BYTE_PARTITION_TABLE 446
#define END_BYTE_PARTITION_TABLE 509
// 每轮读写操作连续读取的扇区数
// 设置 8 或 16。必须是 2 的幂，但不能超过硬盘支持的最大值（IDENTIFY Word 47，通常是16）
#define SECTORS_PER_OP_BLOCK 16

// 统一的逻辑地址转物理地址宏
#define PART_LBA(part, logic_lba) ((part)->start_lba + (logic_lba))

// 封装后的读取宏
#define partition_read(part, logic_lba, buf, count) \
    bread_multi((part)->my_disk, PART_LBA(part, logic_lba), (buf), (count))

// 封装后的写入宏
#define partition_write(part, logic_lba, buf, count) \
    bwrite_multi((part)->my_disk, PART_LBA(part, logic_lba), (buf), (count))

// struct buffer_head* _bread(struct disk* dev, uint32_t lba)
#define bread(part, logic_lba) _bread((part)->my_disk, PART_LBA(part, logic_lba))

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
	// 位图只是我们用来管理 sifs 这个文件系统所使用的结构
	// 他不应该与 part 这个与文件系统无关的数据结构绑定
	// 我们将 bitmap 下放到了 sb->sifs_info 中
	// struct bitmap block_bitmap; 
	// struct bitmap inode_bitmap;
	// 我们使用全局的打开 inode 表，因此此处每个分区的打开 inode 需要给他取消掉
	// struct dlist open_inodes; // inode openned by this partition
};

struct disk{
	char name[8]; // disk name
	struct ide_channel* my_channel; // ide channel that this disk belongs
	uint8_t dev_no; // master ide is 0, slave is 1
	struct partition prim_parts[4]; // the max number of primary partition is 4
	struct partition logic_parts[8]; // we only support 8 logic partition
	// 全盘分区，用于管理没有逻辑分区的裸盘，或者直接绕过分区来对磁盘进行操作
	// 这个分区跨越整个磁盘（从 LBA 0 到最大 LBA）。
	struct partition all_disk_part; 
	uint32_t i_rdev; // 逻辑设备号，用于在vfs中注册时使用
	uint32_t total_sectors;
	struct dlist dirty_lists[2]; // 用于挂载脏扇区头，以便延迟写回
	int32_t active_dirty_idx; // 当前活跃队列的索引 (0 或 1)
    struct lock lists_lock; // 保护该分区队列切换的锁
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

	// DMA 属于通道控制器
	// 在 PCI IDE 控制器规范中，DMA 的寄存器（bmba）是按通道分布的，而不是按磁盘
	// Primary Channel 拥有一套 Bus Master 寄存器（BMBA + 0x00 到 0x07）
	// Secondary Channel 拥有另一套独立的寄存器（BMBA + 0x08 到 0x0F）
	// 每一个通道在同一时刻只能处理一个 DMA 传输
	// 即便在一个通道上挂了两个磁盘（Master 和 Slave），它们也必须共享同一个 DMA 控制器和同一张 PRD 表
	uint32_t bmba; // Bus Master Base Address (从 PCI BAR4 获取)
    bool dma_enabled; // 是否成功开启了 DMA 模式
    struct prd* prd_table; // 该通道专属的 PRDT 表地址，显然它是一个虚拟地址
    uint32_t prd_table_phys;    // 物理地址，用于写到 BMBA 寄存器
};

extern void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt);
extern void ide_init(void);
extern void intr_handler_hd(uint8_t irq_no);
extern void sys_readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size);
extern void sys_read_sectors(const char* hd_name,uint32_t lba, uint8_t* buf, uint32_t sec_cnt);
extern struct partition* get_part_by_rdev(uint32_t rdev);
extern void select_disk(struct disk* hd);
extern void select_sector(struct disk* hd,uint32_t lba,uint8_t sec_cnt);
extern void cmd_out(struct ide_channel* channel,uint8_t cmd);

extern struct ide_channel channels[2];
extern uint8_t channel_cnt;
extern struct dlist partition_list;
extern uint32_t* disk_size;
extern uint8_t disk_num;
struct file_operations ide_file_operations;

#endif