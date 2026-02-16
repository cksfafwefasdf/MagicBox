#include "ide.h"
#include "stdio-kernel.h"
#include "debug.h"
#include "stdio.h"
#include "io.h"
#include "timer.h"
#include "interrupt.h"
#include "string.h"
#include "ide_buffer.h"
#include "global.h"
#include "unistd.h"
#include "device.h"
#include "block_dev.h"
#include "fs.h"

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

#define CMD_SET_MULTIPLE 0xC6 // 设置每次读取的块大小
#define CMD_READ_MULTIPLE 0xC4 // 多扇区读取指令
#define CMD_WRITE_MULTIPLE 0xC5  // 多扇区写入指令

#define max_lba ((80*1024*1024/512)-1) // only surport 80MB disk

// the number of the disk is stored in this addr by BIOS 
#define BIOS_DISK_NUM_ADDR 0x475
#define DISK_PARAM_ADDR 0x501

#define BUSY_WAIT_TIME_LIMIT 30*1000

uint32_t* disk_size;
uint8_t disk_num;

uint8_t channel_cnt;
struct ide_channel channels[CHANNEL_NUM];

// the start lba in the extended partition.
int32_t ext_lba_base = 0;

// the subscript of the main partition and the extended partition
uint8_t p_no=0,l_no=0;
// list of the partition table in extended partition
struct dlist partition_list;

struct partition_table_entry{
	uint8_t bootable;
	uint8_t start_head;
	uint8_t start_sec;
	uint8_t start_chs;
	uint8_t fs_type;
	uint8_t end_head;
	uint8_t end_sec;
	uint8_t end_chs;
	uint32_t start_lba;
	uint32_t sec_cnt;
} __attribute__ ((packed)) ;

// struct of the MBR or EBR
struct boot_sector{
	uint8_t other[446];
	struct partition_table_entry partition_table[4];
	uint16_t signature; // 0x55 0xaa
} __attribute__ ((packed));


static void select_disk(struct disk* hd);
static void select_sector(struct disk* hd,uint32_t lba,uint8_t sec_cnt);
static void cmd_out(struct ide_channel* channel,uint8_t cmd);
static void read_from_sector(struct disk* hd,void* buf,uint8_t sec_cnt);
static void write2sector(struct disk* hd,void* buf,uint8_t sec_cnt);
static bool busy_wait(struct disk* hd);
static void swap_pairs_bytes(const char* dst,char* buf,uint32_t len);
static uint32_t identify_disk(struct disk* hd);
static void partition_scan(struct disk* hd,uint32_t ext_lba);
static bool partition_info(struct dlist_elem* pelem,void* arg UNUSED);


// 定义 IDE 块设备的 file_operations
struct file_operations ide_dev_fops = {
    .read = ide_dev_read,
    .write = ide_dev_write,
    .open = NULL,  // 块设备 open 通常在 sys_open 统一处理
    .close = NULL
};

static void ide_set_multiple_mode(struct disk* hd, uint8_t sec_per_block) {
    select_disk(hd);
    outb(reg_sect_cnt(hd->my_channel), sec_per_block);
    
    // 标记要开始等中断了
    hd->my_channel->expecting_intr = true; 
    outb(reg_cmd(hd->my_channel), CMD_SET_MULTIPLE);
    
    // 阻塞自己，等待中断处理程序执行 sema_signal
    sema_wait(&hd->my_channel->wait_disk); 

    // 被唤醒后，直接检查状态
    uint8_t status = inb(reg_status(hd->my_channel));
    if (status & 0x01) { // 只检查 ERR 位
        uint8_t err = inb(reg_error(hd->my_channel));
        printk("Warning: disk %s Multiple Mode Fail. Err:0x%x\n", hd->name, err);
    } else {
        // 打印成功信息，让你调试时心里有底
        printk("Disk %s: Multiple Mode enabled, %d sectors per block.\n", hd->name, sec_per_block);
    }
}

void intr_handler_hd(uint8_t irq_no){
	ASSERT(irq_no==0x2e||irq_no==0x2f);
	uint8_t ch_no = irq_no-0x2e;
	struct ide_channel* channel = &channels[ch_no];
	ASSERT(channel->irq_no==irq_no);
	// channel->expecting_intr will be true
	// after running cmd_out
	if(channel->expecting_intr){
		channel->expecting_intr = false;
		// Awake the thread.
		// Instead of running instantly, the thead will be put into the ready queue
		sema_signal(&channel->wait_disk);
		// read the status reg to signal the disk controller 
		// that the I/O operation has done,
		// allowing the disk to perform new read/write operations
		inb(reg_status(channel));
	}
}

void ide_init(){
	printk("ide_init start\n");
	dlist_init(&partition_list);
	// get the disk number from BIOS
	uint8_t hd_cnt = *((uint8_t*)(BIOS_DISK_NUM_ADDR));
	ASSERT(hd_cnt>0);
	// each channel has 2 disks
	channel_cnt = DIV_ROUND_UP(hd_cnt,2);

	struct ide_channel* channel;
	uint8_t channel_no = 0;

	while(channel_no<channel_cnt){
		channel = & channels[channel_no];
		sprintf(channel->name,"ide%d",channel_no);

		switch (channel_no){
		case 0:
			channel->port_base = 0x1f0;
			// timer interrption is IRQ0 which intr number is 0x20
			// so the intr number of the IRQ14 is 0x20+14
			channel->irq_no = 0x20+14;
			break;
		case 1:
			channel->port_base = 0x170;
			channel->irq_no = 0x20+15;
			break;
		}

		channel->expecting_intr = false;

		lock_init(&channel->lock);

		// the sema is initialized to 0
		// so that the thread can be blocked by using this sema
		// when the disk controller requests data 
		// when disk intr occur, the intr handler will wakeup 
		// the thread by using this sema.
		sema_init(&channel->wait_disk,0);

		// register the intr handler
		register_handler(channel->irq_no,intr_handler_hd);

		int dev_no = 0;
		while (dev_no<DISK_NUM_IN_CHANNEL){
			// obtain the memory address that the channel has reserved for the disk
			struct disk* hd = &channel->devices[dev_no];
			hd->my_channel = channel;
			hd->dev_no = dev_no;

			// 分配逻辑设备号给磁盘
			int hd_idx = channel_no * 2 + dev_no;
            hd->i_rdev = MAKEDEV(3, hd_idx * 16);

			memset(hd->name,0,sizeof(hd->name));
			sprintf(hd->name,"sd%c",'a'+channel_no*2+dev_no);
			
			uint32_t sectors = identify_disk(hd);
			hd->total_sectors = sectors;

			

			// 初始化全盘分区的基本信息
			memset(&hd->all_disk_part, 0, sizeof(struct partition));
			sprintf(hd->all_disk_part.name, "%s", hd->name); // 例如名字就是 "sda"
			hd->all_disk_part.my_disk = hd;
			hd->all_disk_part.start_lba = 0;
			// identify_disk 时拿到的总扇区数 
			hd->all_disk_part.sec_cnt = sectors; 
			hd->all_disk_part.i_rdev = hd->i_rdev; // 比如 0x30000

			// 将全盘分区也挂载到全局分区链表中
			// 这样 get_part_by_rdev 就能通过链表自动找到它
			dlist_push_back(&partition_list, &hd->all_disk_part.part_tag);

			ide_set_multiple_mode(hd, SECTORS_PER_OP_BLOCK); // 注入设置
			partition_scan(hd,0);
			ext_lba_base = 0;
			p_no=0;
			l_no=0;
			dev_no++;
		}
		dev_no=0;
		channel_no++;
	}

	printk("\tall partition info\n");
	dlist_traversal(&partition_list,partition_info,NULL);

	disk_num = *((uint8_t*)BIOS_DISK_NUM_ADDR);
	disk_size = (uint32_t*)kmalloc(disk_num * sizeof(uint32_t));
	uint32_t* disk_param_addr = DISK_PARAM_ADDR;
	int d_idx = 0;
	for(d_idx=0;d_idx<disk_num;d_idx++){
		disk_size[d_idx] = 0;
		uint32_t ecx = *disk_param_addr++;
		uint32_t edx = *disk_param_addr++;
		// printk("ecx: %x",ecx);
		// printk("edx: %x",edx);
		uint32_t cylinders = (((ecx&0xc0)<<2)|((ecx&0xff00)>>8))+1;
		uint32_t heads = ((edx&0xff00)>>8)+1;
		uint32_t sectors = ecx&0x3f;
		disk_size[d_idx] = cylinders*heads*sectors*SECTOR_SIZE;
	}

	register_block_dev(IDE_MAJOR, &ide_dev_fops, "ide_disk");
	
	printk("ide_init done\n");
	// uint8_t* bf = kmalloc(512);
	// ide_read(&channels[0].devices[0], 0, bf,1);
	// int i=0;
	// for(i=0;i<512;i++){
	// 	printk("%x ",bf[i]);
	// }

}

static void select_disk(struct disk* hd){
	uint8_t reg_device = BIT_DEV_MBS|BIT_DEV_LBA;
	if(hd->dev_no==1){
		// if it is the slave, DEV set to 1
		reg_device|=BIT_DEV_DEV;
	}
	outb(reg_dev(hd->my_channel),reg_device);
}

static void select_sector(struct disk* hd,uint32_t lba,uint8_t sec_cnt){
	if (lba > max_lba) {
        struct task_struct* cur = get_running_task_struct();
        printk("\n[IDE Error] Task:%s, CWD_Inode:%d, LBA:%x\n", 
                cur->name, cur->cwd_inode_nr, lba);
        // 若 CWD_Inode 还是旧的，则 sys_mount 的重置没生效
        ASSERT(lba <= max_lba);
    }
	struct ide_channel* channel = hd->my_channel;

	outb(reg_sect_cnt(channel),sec_cnt);

	// LBA is 28bits
	outb(reg_lba_l(channel),lba);

	outb(reg_lba_m(channel),lba>>8);
	outb(reg_lba_h(channel),lba>>16);
	// LBA 23~27 bits should be wrote to DEV reg
	outb(reg_dev(channel),BIT_DEV_MBS|BIT_DEV_LBA|(hd->dev_no==1?BIT_DEV_DEV:0)|lba>>24);

}

static void cmd_out(struct ide_channel* channel,uint8_t cmd){
	// the disk starts working after write cmd to cmd-reg
	channel->expecting_intr = true;
	outb(reg_cmd(channel),cmd);
}



static void read_from_sector(struct disk* hd,void* buf,uint8_t sec_cnt){
	uint32_t size_in_byte;
	if(sec_cnt==0){
		// sec_cnt = 0 means 256 sectors
		// otherwise sec_cnt = 0 is meanningless
		size_in_byte = 256*SECTOR_SIZE;
	}else{
		size_in_byte = sec_cnt*SECTOR_SIZE;
	}
	// Since we write 16 bits at a time, [size_in_byte] should divide by 2
	insw(reg_data(hd->my_channel),buf,size_in_byte/2);
}

static void write2sector(struct disk* hd,void* buf,uint8_t sec_cnt){
	uint32_t size_in_byte;
	if(sec_cnt==0){
		size_in_byte = 256*SECTOR_SIZE;
	}else{
		size_in_byte = sec_cnt * SECTOR_SIZE;
	}
	outsw(reg_data(hd->my_channel),buf,size_in_byte/2);
}

// busy wati 30 seconds
// static bool busy_wait(struct disk* hd){
// 	struct ide_channel* channel = hd->my_channel;
// 	uint16_t time_limit = BUSY_WAIT_TIME_LIMIT;
// 	while(time_limit-=10>0){
// 		if(!(inb(reg_status(channel))&BIT_ALT_STAT_BSY)){
// 			// DRQ is 1 means disk is ready to read or write
// 			return (inb(reg_status(channel))&BIF_ALT_STAT_DRQ);
// 		}else{
// 			mtime_sleep(10); //sleep 10ms, this thread will yield the CPU
// 		}
// 	}
// 	// All actions required in this state shall be completed within 31 s
// 	// if the disk fails to complete the operation within 30 seconds
// 	// return false
// 	return false;
// }

static bool busy_wait(struct disk* hd) {
    struct ide_channel* channel = hd->my_channel;
    // 轮询一段时间（比如 100,000 次），不进行 thread_yield
    // 因为 QEMU 模拟器里，磁盘状态翻转极快，原地等待可能只要几百个纳秒
    uint32_t timeout = 1000000; 
    while (timeout--) {
        uint8_t status = inb(reg_status(channel));
        if (!(status & BIT_ALT_STAT_BSY)) { // 不忙了
            return (status & BIF_ALT_STAT_DRQ); // 返回是否准备好数据
        }
    }
    return false;
}

void ide_read(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    lock_acquire(&hd->my_channel->lock);

    // 告知起始地址和总扇区数
    select_sector(hd, lba, sec_cnt); 
    
    // 发送多扇区读指令
    cmd_out(hd->my_channel, CMD_READ_MULTIPLE);

    uint32_t secs_done = 0;
    while(secs_done < sec_cnt) {
        // 标记期待中断并睡眠
        hd->my_channel->expecting_intr = true;
        sema_wait(&hd->my_channel->wait_disk); 

        // 计算本次中断触发后，硬盘缓冲区里准备好了多少扇区
        // 如果剩余扇区数大于 Block 因子，则说明缓冲区里有整整一 Block
        // 如果是最后一次中断，则读取剩下的所有扇区
        uint32_t left_secs = sec_cnt - secs_done;
        uint32_t secs_to_read = (left_secs < SECTORS_PER_OP_BLOCK) ? left_secs : SECTORS_PER_OP_BLOCK;

        // 一口气用 insw 抽走这些数据（这是最快的地方）
        read_from_sector(hd, (void*)((uint32_t)buf + secs_done * SECTOR_SIZE), secs_to_read);
        
        secs_done += secs_to_read;
    }

    lock_release(&hd->my_channel->lock);
}


void ide_write(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    lock_acquire(&hd->my_channel->lock);

    // 告知起始地址和总扇区数
    select_sector(hd, lba, sec_cnt);
    
    // 发送多扇区写指令
    cmd_out(hd->my_channel, CMD_WRITE_MULTIPLE);

    uint32_t secs_done = 0;
    while (secs_done < sec_cnt) {
        // 在开始写入第一块之前，必须确认硬盘已经准备好接收数据（BSY=0, DRQ=1）
        if (!busy_wait(hd)) {
            PANIC("ide_write: busy_wait timeout before write!");
        }

        // 计算本次要写入多少扇区（通常是 16 个，最后一次可能少于 16）
        uint32_t left_secs = sec_cnt - secs_done;
        uint32_t secs_to_write = (left_secs < SECTORS_PER_OP_BLOCK) ? left_secs : SECTORS_PER_OP_BLOCK;

        // 这里的 write2sector 内部会调用 outsw，一口气把数据刷进硬盘缓冲区
        write2sector(hd, (void*)((uint32_t)buf + secs_done * SECTOR_SIZE), secs_to_write);

        // 写入一个 Block 后，硬盘会开始处理物理落盘，处理完后会发中断
        hd->my_channel->expecting_intr = true;
        sema_wait(&hd->my_channel->wait_disk);

        secs_done += secs_to_write;
    }

    lock_release(&hd->my_channel->lock);
}


static void swap_pairs_bytes(const char* dst,char* buf,uint32_t len){
	uint8_t idx;
	for(idx=0;idx<len;idx+=2){
		buf[idx+1] = *dst++;
		buf[idx] = *dst++;
	}
	buf[idx] = '\0';
}

static uint32_t identify_disk(struct disk* hd){
	char id_info[512];
	select_disk(hd);
	cmd_out(hd->my_channel,CMD_IDENTIFY);

	sema_wait(&hd->my_channel->wait_disk);

	if(!busy_wait(hd)){
		char error[64];
		sprintf(error,"%s identify failed!!!!!!!\n",hd->name);
		PANIC(error);
	}
	read_from_sector(hd,id_info,1);
	
	char buf[64];
	// printk("disk_name: %s\n",hd->name);
	uint8_t sn_start = 10*2,sn_len = 20,md_start = 27*2,md_len = 40;
	swap_pairs_bytes(&id_info[sn_start],buf,sn_len);
	printk("	disk %s info:\n\t\tSN:%s\n",hd->name,buf);
	memset(buf,0,sizeof(buf));
	swap_pairs_bytes(&id_info[md_start],buf,md_len);
	printk("\t\tMODULE: %s\n",buf);
	uint32_t sectors = *(uint32_t*)&id_info[60*2];
	printk("\t\tSECTORS: %d\n",sectors);
	printk("\t\tCAPACITY: %dMB\n",sectors*512/1024/1024);
	return sectors;
}

static void partition_scan(struct disk* hd,uint32_t ext_lba){
	struct boot_sector* bs = kmalloc(sizeof(struct boot_sector));
	bread_multi(hd,ext_lba,bs,1);
	uint8_t part_idx = 0;
	struct partition_table_entry* p = bs->partition_table;

	while (part_idx++<4){ 
		if(p->fs_type==0x5){ // 扩展分区
			if(ext_lba_base!=0){
				partition_scan(hd,p->start_lba+ext_lba_base);
			}else{
				ext_lba_base = p->start_lba;
				partition_scan(hd,p->start_lba);
			}
		}else if(p->fs_type!=0){ // 有效分区
			if(ext_lba==0){ // 主分区
				hd->prim_parts[p_no].start_lba = ext_lba+p->start_lba;
				hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
				hd->prim_parts[p_no].my_disk = hd;
				
				// 分配逻辑设备号：母盘设备号 + (1~4)
                hd->prim_parts[p_no].i_rdev = hd->i_rdev + p_no + 1;
				
				dlist_push_back(&partition_list,&hd->prim_parts[p_no].part_tag);
				sprintf(hd->prim_parts[p_no].name,"%s%d",hd->name,p_no+1);
				p_no++;
				ASSERT(p_no<4);

			}else{ // 逻辑分区
				hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
				hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
				hd->logic_parts[l_no].my_disk = hd;
				
				// 分配设备号：母盘设备号 + (5~12)
                hd->logic_parts[l_no].i_rdev = hd->i_rdev + l_no + 5;
				
				dlist_push_back(&partition_list,&hd->logic_parts[l_no].part_tag);
				sprintf(hd->logic_parts[l_no].name,"%s%d",hd->name,l_no+5);
				l_no++;
				if(l_no>=8) return;
			}
		}
		p++;
	}
	kfree(bs);
}

static bool partition_info(struct dlist_elem* pelem,void* arg UNUSED){
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	printk("\t\t%s start_lba:0x%x, sec_cnt:0x%x\n",part->name,part->start_lba,part->sec_cnt);
	return false;
}

// 流式读取的readraw，防止文件过大导致堆空间被迅速耗尽
void sys_readraw(const char* disk_name, uint32_t lba, const char* filename, uint32_t file_size) {
    struct disk* disk;
    if(!strcmp("sda", disk_name)) {
        disk = &channels[0].devices[0];
    } else if(!strcmp("sdb", disk_name)) {
        disk = &channels[0].devices[1];
    } else {
        printk("unknown disk name!\n");
        return;
    }

    // 动态计算缓冲区大小
    uint32_t max_buf_size = PG_SIZE*4;
    uint32_t buf_size;

    if (file_size < max_buf_size) {
        // 如果文件小于 16KB，按扇区大小向上对齐分配
        // 比如文件 600 字节，分配 1024 字节，防止磁盘驱动写越界
        buf_size = DIV_ROUND_UP(file_size, SECTOR_SIZE) * SECTOR_SIZE;
    } else {
        buf_size = max_buf_size;
    }

    void* buf = kmalloc(buf_size);
    if (buf == NULL) {
        printk("sys_readraw: kmalloc failed!\n");
        return;
    }

    int32_t fd = sys_open(filename, O_CREATE | O_RDWR);
    if (fd == -1) {
        kfree(buf);
        return;
    }

    uint32_t bytes_left = file_size;
    uint32_t current_lba = lba;

    // 循环读写
    while (bytes_left > 0) {
        // 本次实际要写回文件系统的字节数
        uint32_t bytes_to_write = (bytes_left > buf_size) ? buf_size : bytes_left;
        
        // 本次从磁盘读取的扇区数（必须是整数个扇区）
        uint32_t secs_to_read = DIV_ROUND_UP(bytes_to_write, SECTOR_SIZE);

        bread_multi(disk, current_lba, buf, secs_to_read);

        if (sys_write(fd, buf, bytes_to_write) == -1) {
            printk("sys_readraw: write error!\n");
            break;
        }

        bytes_left -= bytes_to_write;
        current_lba += secs_to_read;
    }

    kfree(buf);
    sys_close(fd);
}

// 对 bread_multi 在用户层面上的封装
void sys_read_sectors(const char* hd_name, uint32_t lba, uint8_t* buf, uint32_t sec_cnt) {
	struct disk* disk;
    if(!strcmp("sda", hd_name)) {
        disk = &channels[0].devices[0];
    } else if(!strcmp("sdb", hd_name)) {
        disk = &channels[0].devices[1];
    } else {
        printk("unknown disk name: %s!\n", hd_name);
        return;
    }

    bread_multi(disk, lba, buf, sec_cnt);
}

// 磁盘设备 VFS 读取接口
// file VFS 文件结构
// buf 用户缓冲区
// count 读取字节数
int32_t ide_dev_read(struct file* file, void* buf, uint32_t count) {
    struct m_inode* inode = file->fd_inode;
	printk("inode->di.i_rdev:%x\n",inode->di.i_rdev);
    struct partition* part = get_part_by_rdev(inode->di.i_rdev);
    uint32_t part_size_bytes = part->sec_cnt * SECTOR_SIZE;

	// 边界检查
	// 在磁盘中，fd_pos就表示现在要操作磁盘的第几个字节
	// 我们要操作的位置不能超过整个分区的大小
    if (file->fd_pos >= part_size_bytes) return -1;
	// 如果读的字节数太多，超过分区大小了，那么就进行截断，只读到分区的最后一个字节处
    if (file->fd_pos + count > part_size_bytes) {
        count = part_size_bytes - file->fd_pos;
    }

    uint8_t* dst = (uint8_t*)buf;
    uint32_t bytes_left = count;
    uint8_t* io_buf = NULL; // 仅用于处理非对齐碎块

	// 5个字节一个扇区00111对应 offset_in_sec != 0分支
	// 11100 对应 bytes_left < SECTOR_SIZE 分支
	// 00110 对应两者同时成立的分支
	// 00111 11111 11111 11100 
	// 00000 00110 00000 00000
    while (bytes_left > 0) {
        uint32_t lba = part->start_lba + (file->fd_pos / SECTOR_SIZE);
        uint32_t offset_in_sec = file->fd_pos % SECTOR_SIZE;

        // 先处理非对齐读取（处理起始位置不在 512 字节边界，或者最后一个扇区剩余长度不足一扇区）
        if (offset_in_sec != 0 || bytes_left < SECTOR_SIZE) {
            if (!io_buf) io_buf = kmalloc(SECTOR_SIZE);
            
            // 由于并不足一扇区，一次就处理一扇区
            bread_multi(part->my_disk, lba, io_buf, 1);
            
			// 从第一个有效字节偏移地址往高地址走（从左往右走），剩下的字节数
            uint32_t left_in_sec = SECTOR_SIZE - offset_in_sec;

            // bytes_left 用于判断当前的情况是最开始的其实位置不在 512 字节边界
			// 还是最后一个扇区待读的数据不足 512 字节
			// 若 bytes_left >= left_in_sec ，则说明是前一种情况，起始位置不在 512 字节边界，本次读取的数据大小是 left_in_sec
			// 若 bytes_left < left_in_sec 则说是后一种情况，最后一个扇区待读数据不足 512 字节，本次读取的数据大小是 bytes_left
			uint32_t chunk_size = (bytes_left < left_in_sec) ? bytes_left : left_in_sec;
            
            memcpy(dst, io_buf + offset_in_sec, chunk_size);
            
            file->fd_pos += chunk_size;
            bytes_left -= chunk_size;
            dst += chunk_size;
        } else { // 对齐读取（光标在边界，且剩余长度至少有一个整扇区）
            // 计算目前能进行的最大批量读取扇区数
            uint32_t secs_to_read = bytes_left / SECTOR_SIZE;

            bread_multi(part->my_disk, lba, dst, secs_to_read);
            
            uint32_t total_size = secs_to_read * SECTOR_SIZE;
            file->fd_pos += total_size;
            bytes_left -= total_size;
            dst += total_size;
        }
    }

    if (io_buf) kfree(io_buf);
    return (int32_t)count;
}

// 磁盘设备 VFS 读取接口
// 参数含义同 ide_dev_read
int32_t ide_dev_write(struct file* file, const void* buf, uint32_t count) {
    struct m_inode* inode = file->fd_inode;
    struct partition* part = get_part_by_rdev(inode->di.i_rdev);
    uint32_t part_size_bytes = part->sec_cnt * SECTOR_SIZE;

    // 边界检查
    if (file->fd_pos >= part_size_bytes) return -1;
	// 同样截断多余的部分
    if (file->fd_pos + count > part_size_bytes) {
        count = part_size_bytes - file->fd_pos;
    }

    const uint8_t* src = (const uint8_t*)buf;
    uint32_t bytes_left = count;
    uint8_t* io_buf = NULL; // 用于处理非对齐部分

    while (bytes_left > 0) {
        uint32_t lba = part->start_lba + (file->fd_pos / SECTOR_SIZE);
        uint32_t offset_in_sec = file->fd_pos % SECTOR_SIZE;

        // 非对齐写入（起始位置不对齐，或剩余数据不足一扇区）
        // 此时必须执行 RMW (Read-Modify-Write)，否则会破坏扇区内的其他数据
        if (offset_in_sec != 0 || bytes_left < SECTOR_SIZE) {
            if (!io_buf) io_buf = kmalloc(SECTOR_SIZE);

            // Read，先把旧数据读出来
            bread_multi(part->my_disk, lba, io_buf, 1);

            // Modify，覆盖 io_buf 中的特定部分
            uint32_t left_in_sec = SECTOR_SIZE - offset_in_sec;
            uint32_t chunk_size = (bytes_left < left_in_sec) ? bytes_left : left_in_sec;
            memcpy(io_buf + offset_in_sec, src, chunk_size);

            // Write，写回整扇区 (利用你的 bwrite_multi 物理写 + 缓存更新)
            bwrite_multi(part->my_disk, lba, io_buf, 1);

            file->fd_pos += chunk_size;
            bytes_left -= chunk_size;
            src += chunk_size;
        } else { // 对齐写入（光标在边界，且待写数据至少包含一个整扇区）
            // 计算目前能进行的最大批量对齐写入扇区数
            uint32_t secs_to_write = bytes_left / SECTOR_SIZE;

            // 直接批量写入
            bwrite_multi(part->my_disk, lba, (void*)src, secs_to_write);

            uint32_t total_size = secs_to_write * SECTOR_SIZE;
            file->fd_pos += total_size;
            bytes_left -= total_size;
            src += total_size;
        }
    }

    if (io_buf) kfree(io_buf);
    return (int32_t)count;
}

// 通过 vfs 逻辑设备号拿到 partition
struct partition* get_part_by_rdev(uint32_t rdev) {
    struct dlist_elem* elem = partition_list.head.next;
    while (elem != &partition_list.tail) {
        struct partition* part = member_to_entry(struct partition, part_tag, elem);
        if (part->i_rdev == rdev) {
            return part;
        }
        elem = elem->next;
    }

    return NULL; 
}