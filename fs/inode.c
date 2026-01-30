#include "inode.h"
#include "ide.h"
#include "stdint.h"
#include "debug.h"
#include "dlist.h"
#include "string.h"
#include "fs.h"
#include "super_block.h"
#include "thread.h"
#include "interrupt.h"
#include "stdio-kernel.h"
#include "file.h"
#include "ide_buffer.h"

struct inode_position{
	bool two_sec; // whether an inode spans multiple sectors
	uint32_t sec_lba;
	uint32_t off_size; // byte offset of the inode within the sectors
};

// Calculate the logical sector offset and byte offset within the sector by inode number
static void inode_locate(struct partition* part,uint32_t inode_no,struct inode_position* inode_pos){
	ASSERT(inode_no<MAX_FILES_PER_PART);
	uint32_t inode_table_lba = part->sb->inode_table_lba;

	uint32_t inode_size = sizeof(struct inode);
	// total bytes offset
	uint32_t off_size = inode_no*inode_size;

	uint32_t off_sec = off_size/SECTOR_SIZE;
	// offset in sectors
	uint32_t off_size_in_sec = off_size%SECTOR_SIZE;
	// remaining sector space from off_size_in_sec
	uint32_t left_in_sec = SECTOR_SIZE-off_size_in_sec;
	// check if the inode spans two sectors
	if(left_in_sec<inode_size){
		inode_pos->two_sec = true;
	}else{
		inode_pos->two_sec = false;
	}
	inode_pos->sec_lba = inode_table_lba+off_sec;
	inode_pos->off_size = off_size_in_sec;
}

// Write the inode back to disk
// Sync the inode to disk
void inode_sync(struct partition* part,struct inode* inode,void* io_buf){

	uint8_t inode_no = inode->i_no;
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);

	ASSERT(inode_pos.sec_lba<=(part->start_lba+part->sec_cnt));

	struct inode pure_inode;
	memcpy(&pure_inode,inode,sizeof(struct inode));

	pure_inode.i_open_cnts = 0;
	pure_inode.write_deny = false;

	pure_inode.inode_tag.prev = pure_inode.inode_tag.next=NULL;

	char* inode_buf = (char*)io_buf;

	if(inode_pos.two_sec){
		// printk("WARNING: Inode spans two sectors! lba:%d\n", inode_pos.sec_lba);
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,2);
		memcpy((inode_buf+inode_pos.off_size),&pure_inode,sizeof(struct inode));
		bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,2);
	}else{
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,1);
		memcpy((inode_buf+inode_pos.off_size),&pure_inode,sizeof(struct inode));
		bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,1);
	}

}

// load the inode from disk into memory
struct inode* inode_open(struct partition* part,uint32_t inode_no){
	enum intr_status old_status = intr_disable();

	struct dlist_elem* elem = part->open_inodes.head.next;
	struct inode* inode_found;
	while(elem!=&part->open_inodes.tail){
		inode_found = member_to_entry(struct inode,inode_tag,elem);
		if(inode_found->i_no==inode_no){
			inode_found->i_open_cnts++;
			return inode_found;
		}
		elem = elem->next;
	}

	

	struct inode_position inode_pos;

	inode_locate(part,inode_no,&inode_pos);

	struct task_struct* cur = get_running_task_struct();

	inode_found = (struct inode*)kmalloc(sizeof(struct inode));
	
	if (inode_found==NULL){
		PANIC("alloc memory failed!");
	}
	

	char* inode_buf;
	
	if(inode_pos.two_sec){
		// printk("WARNING: Inode spans two sectors! lba:%d\n", inode_pos.sec_lba);
		inode_buf = (char*)kmalloc(SECTOR_SIZE*2);
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,2);
	}else{
		inode_buf = (char*)kmalloc(SECTOR_SIZE);
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,1);
	}
	memcpy(inode_found,inode_buf+inode_pos.off_size,sizeof(struct inode));
	
	dlist_push_front(&part->open_inodes,&inode_found->inode_tag);
	
	inode_found->i_open_cnts=1;
	// ASSERT((uint32_t)inode_found>=K_HEAP_START);
	
	kfree(inode_buf);
	// printk("inode flag::: %x\n",inode_found->write_deny);
	intr_set_status(old_status);
	return inode_found;
}

void inode_close(struct inode* inode){
	enum intr_status old_status = intr_disable();
	if(--inode->i_open_cnts==0){
		dlist_remove(&inode->inode_tag);
		
		struct task_struct* cur = get_running_task_struct();
		// printk("inode_close:::inode addr: %x\n",inode);
		kfree(inode);
	}
	intr_set_status(old_status);
}

void inode_init(uint32_t inode_no,struct inode* new_inode,enum file_types ft){
	new_inode->i_no = inode_no;
	new_inode->i_size = 0;
	new_inode->i_open_cnts = 0;
	new_inode->write_deny = false;
	new_inode->i_type = ft;

	uint8_t sec_idx = 0;
	while(sec_idx<BLOCK_PTR_NUMBER){
		new_inode->i_sectors[sec_idx]=0;
		sec_idx++;
	}
}

void inode_delete(struct partition* part,uint32_t inode_no,void* io_buf){
	ASSERT(inode_no<MAX_FILES_PER_PART);
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);
	ASSERT(inode_pos.sec_lba<=(part->start_lba+part->sec_cnt));

	char* inode_buf = (char*)io_buf;
	if(inode_pos.two_sec){
		// printk("WARNING: Inode spans two sectors! lba:%d\n", inode_pos.sec_lba);
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,2);
		memset((inode_buf+inode_pos.off_size),0,sizeof(struct inode));
		bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,2);
	}else{
		bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,1);
		memset((inode_buf+inode_pos.off_size),0,sizeof(struct inode));
		bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,1);
	}
}

void inode_release(struct partition* part,uint32_t inode_no){
	struct inode* inode_to_del = inode_open(part,inode_no);
	ASSERT(inode_to_del->i_no==inode_no);

	uint8_t block_idx = 0,block_cnt = DIRECT_INDEX_BLOCK;
	uint32_t block_bitmap_idx;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};

	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = inode_to_del->i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(inode_to_del->i_sectors[tfflib]!=0){
		bread_multi(part->my_disk,inode_to_del->i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;

		block_bitmap_idx = inode_to_del->i_sectors[tfflib] - part->sb->data_start_lba;
		ASSERT(block_bitmap_idx>0);
		bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
		bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);
	}

	block_idx = 0;
	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]!=0){
			block_bitmap_idx = 0;
			block_bitmap_idx = all_blocks_addr[block_idx]-part->sb->data_start_lba;
			ASSERT(block_bitmap_idx>0);
			bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
			bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);
		}
		block_idx++;
	}

	bitmap_set(&part->inode_bitmap,inode_no,0);
	bitmap_sync(cur_part,inode_no,INODE_BITMAP);

	void* io_buf = kmalloc(SECTOR_SIZE*2);
	inode_delete(part,inode_no,io_buf);
	kfree(io_buf);

	inode_close(inode_to_del);
}
	 