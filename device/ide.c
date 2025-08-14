#include "ide.h"
#include "stdint.h"
#include "stdio-kernel.h"
#include "debug.h"
#include "stdio.h"
#include "io.h"
#include "timer.h"
#include "interrupt.h"
#include "string.h"
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

#define max_lba ((80*1024*1024/512)-1) // only surport 80MB disk

// the number of the disk is stored in this addr by BIOS 
#define BIOS_DISK_NUM_ADDR 0x475

#define BUSY_WAIT_TIME_LIMIT 30*1000



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
static void identify_disk(struct disk* hd);
static void partition_scan(struct disk* hd,uint32_t ext_lba);
static bool partition_info(struct dlist_elem* pelem,int arg UNUSED);


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
			memset(hd->name,0,sizeof(hd->name));
			sprintf(hd->name,"sd%c",'a'+channel_no*2+dev_no);
			identify_disk(hd);
			if(dev_no!=0){
				partition_scan(hd,0);
			}
			p_no=0,l_no=0;
			dev_no++;
		}
		dev_no=0;
		channel_no++;
	}

	printk("\tall partition info\n");
	dlist_traversal(&partition_list,partition_info,(int)NULL);

	printk("ide_init done\n");
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
	ASSERT(lba<=max_lba);
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
		size_in_byte = 256*SEC_SIZE;
	}else{
		size_in_byte = sec_cnt*SEC_SIZE;
	}
	// Since we write 16 bits at a time, [size_in_byte] should divide by 2
	insw(reg_data(hd->my_channel),buf,size_in_byte/2);
}

static void write2sector(struct disk* hd,void* buf,uint8_t sec_cnt){
	uint32_t size_in_byte;
	if(sec_cnt==0){
		size_in_byte = 256*SEC_SIZE;
	}else{
		size_in_byte = sec_cnt * SEC_SIZE;
	}
	outsw(reg_data(hd->my_channel),buf,size_in_byte/2);
}

// busy wati 30 seconds
static bool busy_wait(struct disk* hd){
	struct ide_channel* channel = hd->my_channel;
	uint16_t time_limit = BUSY_WAIT_TIME_LIMIT;
	while(time_limit-=10>0){
		if(!(inb(reg_status(channel))&BIT_ALT_STAT_BSY)){
			// DRQ is 1 means disk is ready to read or write
			return (inb(reg_status(channel))&BIF_ALT_STAT_DRQ);
		}else{
			mtime_sleep(10); //sleep 10ms, this thread will yield the CPU
		}
	}
	// All actions required in this state shall be completed within 31 s
	// if the disk fails to complete the operation within 30 seconds
	// return false
	return false;
}

void ide_read(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt){
	ASSERT(lba<=max_lba);
	ASSERT(sec_cnt>0);
	lock_acquire(&hd->my_channel->lock);

	// to indicate which disk will we use, master or slave
	select_disk(hd);

	uint32_t secs_op; // the number of sector in each operation
	uint32_t secs_done = 0; // the sectors that we have processed.
	while(secs_done<sec_cnt){
		if((secs_done+256)<=sec_cnt){
			// if the number of the sector that we haven't processed exceeds 256
			// then the secs_op is 256
			secs_op = 256;
		}else{
			secs_op = sec_cnt - secs_done;
		}
		// write LBA regs
		select_sector(hd,lba+secs_done,secs_op);
		// write command, start IO
		cmd_out(hd->my_channel,CMD_READ_SECTOR);
		// block this thread until the disk intr occurs
		sema_wait(&hd->my_channel->wait_disk);
		
		// the operation below will be executed when this thread is awakened up by disk intr
		if(!busy_wait(hd)){
			char error[64];
			sprintf(error,"%s read sector %d failed !!!\n",hd->name,lba);
			PANIC(error);
		}

		read_from_sector(hd,(void*)((uint32_t)buf+secs_done*512),secs_op);
		secs_done+=secs_op;
	}
	lock_release(&hd->my_channel->lock);

}

// the procedures of ide_write are the same as ide_read
void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt){
	ASSERT(lba<=max_lba);
	ASSERT(sec_cnt>0);
	lock_acquire(&hd->my_channel->lock);

	select_disk(hd);

	uint32_t secs_op;
	uint32_t secs_done = 0;
	while(secs_done<sec_cnt){
		if((secs_done+256)<=sec_cnt){
			secs_op = 256;
		}else{
			secs_op =sec_cnt - secs_done;
		}
		select_sector(hd,lba+secs_done,secs_op);

		cmd_out(hd->my_channel,CMD_WRITE_SECTOR);

		if(!busy_wait(hd)){
			char error[64];
			sprintf(error,"%s write sector %d failed !!!\n",hd->name,lba);
			PANIC(error);
		}

		write2sector(hd,(void*)((uint32_t)buf+secs_done*512),secs_op);
		// block this thread while writing data to the disk
		sema_wait(&hd->my_channel->wait_disk);

		secs_done += secs_op;
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

static void identify_disk(struct disk* hd){
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
}

static void partition_scan(struct disk* hd,uint32_t ext_lba){
	struct boot_sector* bs = sys_malloc(sizeof(struct boot_sector));
	ide_read(hd,ext_lba,bs,1);
	uint8_t part_idx = 0;
	struct partition_table_entry* p = bs->partition_table;

	while (part_idx++<4){
		if(p->fs_type==0x5){
			if(ext_lba_base!=0){
				partition_scan(hd,p->start_lba+ext_lba_base);
			}else{
				ext_lba_base = p->start_lba;
				partition_scan(hd,p->start_lba);
			}
		}else if(p->fs_type!=0){
			if(ext_lba==0){
				hd->prim_parts[p_no].start_lba = ext_lba+p->start_lba;
				hd->prim_parts[p_no].sec_cnt = p->sec_cnt;
				hd->prim_parts[p_no].my_disk = hd;
				dlist_push_back(&partition_list,&hd->prim_parts[p_no].part_tag);
				sprintf(hd->prim_parts[p_no].name,"%s%d",hd->name,p_no+1);
				p_no++;
				ASSERT(p_no<4);

			}else{
				hd->logic_parts[l_no].start_lba = ext_lba + p->start_lba;
				hd->logic_parts[l_no].sec_cnt = p->sec_cnt;
				hd->logic_parts[l_no].my_disk = hd;
				dlist_push_back(&partition_list,&hd->logic_parts[l_no].part_tag);
				sprintf(hd->logic_parts[l_no].name,"%s%d",hd->name,l_no+5);
				l_no++;
				if(l_no>=8) return;
			}
		}
		p++;
	}
	sys_free(bs);
}

static bool partition_info(struct dlist_elem* pelem,int arg UNUSED){
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	printk("\t\t%s start_lba:0x%x, sec_cnt:0x%x\n",part->name,part->start_lba,part->sec_cnt);
	return false;
}

// read the disk without file system
void sys_readraw(const char* disk_name,uint32_t lba,const char* filename,uint32_t file_size){
	struct disk* disk;
	if(!strcmp("sda",disk_name)){
		disk = &channels[0].devices[0];
	}else if(!strcmp("sdb",disk_name)){
		disk = &channels[0].devices[1];
	}else{
		printf("unknown disk name!\n");
		return ;
	}
	uint32_t sec_cnt =  DIV_ROUND_UP(file_size,SECTOR_SIZE);
	void* buf =  sys_malloc(sec_cnt*SECTOR_SIZE);
	if(buf==NULL){
		printf("sys_readraw: sys_malloc for buf failed!\n");
		return ;
	}
   	ide_read(disk,lba,buf,sec_cnt);
	int32_t fd = sys_open(filename,O_CREATE|O_RDWR);

	if(fd!=-1){
		if(sys_write(fd,buf,file_size)==-1){
			printk("file write error!\n");
			while(1);
		}
	}
	sys_free(buf);
	sys_close(fd);
}


