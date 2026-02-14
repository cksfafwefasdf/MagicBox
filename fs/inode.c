#include "inode.h"
#include "ide.h"
#include "stdint.h"
#include "debug.h"
#include "dlist.h"
#include "string.h"
#include "fs.h"
#include "thread.h"
#include "interrupt.h"
#include "stdio-kernel.h"
#include "file.h"
#include "ide_buffer.h"
#include "fs_types.h"

struct inode_position{
	bool two_sec; // whether an inode spans multiple sectors
	uint32_t sec_lba;
	uint32_t off_size; // byte offset of the inode within the sectors
};

// Calculate the logical sector offset and byte offset within the sector by inode number
static void inode_locate(struct partition* part,uint32_t inode_no,struct inode_position* inode_pos){
	ASSERT(inode_no<MAX_FILES_PER_PART);
	uint32_t inode_table_lba = part->sb->inode_table_lba;

	uint32_t inode_size = sizeof(struct d_inode);
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
void inode_sync(struct partition* part,struct m_inode* inode,void* io_buf){
	// printk("inode->i_dev:%x  part->i_rdev:%x \n",inode->i_dev,part->i_rdev);
	ASSERT(inode->i_dev == part->i_rdev);
	uint8_t inode_no = inode->i_no;
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);

	ASSERT(inode_pos.sec_lba<=(part->start_lba+part->sec_cnt));

	char* inode_buf = (char*)io_buf;
	uint8_t sec_to_write = inode_pos.two_sec?2:1;

	bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,sec_to_write);
	memcpy((inode_buf+inode_pos.off_size),&inode->di,sizeof(struct d_inode));
	bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,sec_to_write);

}

// load the inode from disk into memory
struct m_inode* inode_open(struct partition* part,uint32_t inode_no){
	enum intr_status old_status = intr_disable();

	struct dlist_elem* elem = part->open_inodes.head.next;
	struct m_inode* inode_found;
	while(elem!=&part->open_inodes.tail){
		inode_found = member_to_entry(struct m_inode,inode_tag,elem);
		if(inode_found->i_no==inode_no){
			inode_found->i_open_cnts++;
			intr_set_status(old_status);
			return inode_found;
		}
		elem = elem->next;
	}

	

	struct inode_position inode_pos;

	inode_locate(part,inode_no,&inode_pos);

	inode_found = (struct m_inode*)kmalloc(sizeof(struct m_inode));
	
	if (inode_found==NULL){
		PANIC("alloc memory failed!");
	}
	
	uint8_t sec_to_read = inode_pos.two_sec?2:1;
	
	char* inode_buf = (char*)kmalloc(SECTOR_SIZE*sec_to_read);
	bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,sec_to_read);
	
	memcpy(&inode_found->di,inode_buf+inode_pos.off_size,sizeof(struct d_inode));

	inode_found->i_no = inode_no;
	inode_found->i_dev = part->i_rdev;
	inode_found->i_open_cnts = 1;
	inode_found->write_deny = false;
	
	dlist_push_front(&part->open_inodes,&inode_found->inode_tag);
	
	ASSERT((uint32_t)inode_found>=K_HEAP_START);
	
	kfree(inode_buf);
	// printk("inode flag::: %x\n",inode_found->write_deny);
	intr_set_status(old_status);
	return inode_found;
}

void inode_close(struct m_inode* inode){
	enum intr_status old_status = intr_disable();
	if(--inode->i_open_cnts==0){

		// 对 FIFO 和 PIPE 的缓冲区进行回收
		// 让缓冲区随inode的消亡同时消亡，保证强一致性
        if (inode->di.i_type == FT_FIFO || inode->di.i_type == FT_PIPE) {
            if (inode->di.i_pipe_ptr != 0) {
                mfree_page(PF_KERNEL, (void*)inode->di.i_pipe_ptr, 1);
                inode->di.i_pipe_ptr = 0;
            }
        }
		// 非匿名管道再进行释放，匿名inode是不会出现在打开队列上的
		if(inode->i_no!=ANONY_I_NO)
			dlist_remove(&inode->inode_tag);

		kfree(inode);
	}
	intr_set_status(old_status);
}

void inode_init(struct partition* part, uint32_t inode_no,struct m_inode* new_inode,enum file_types ft){
	new_inode->i_no = inode_no;
	new_inode->di.i_size = 0;
	new_inode->i_open_cnts = 0;
	new_inode->write_deny = false;
	new_inode->di.i_type = ft;
	new_inode->i_dev = part->i_rdev;

	uint8_t sec_idx = 0;
	while(sec_idx<BLOCK_PTR_NUMBER){
		new_inode->di.i_sectors[sec_idx]=0;
		sec_idx++;
	}
}

void inode_delete(struct partition* part,uint32_t inode_no,void* io_buf){
	ASSERT(inode_no<MAX_FILES_PER_PART);
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);
	ASSERT(inode_pos.sec_lba<=(part->start_lba+part->sec_cnt));

	char* inode_buf = (char*)io_buf;
	uint8_t sec_to_op = inode_pos.two_sec?2:1;

	bread_multi(part->my_disk,inode_pos.sec_lba,inode_buf,sec_to_op);
	memset((inode_buf+inode_pos.off_size),0,sizeof(struct d_inode));
	bwrite_multi(part->my_disk,inode_pos.sec_lba,inode_buf,sec_to_op);
	
}

// 只有这些类型才需要回收磁盘块
static bool has_data_blocks(enum file_types type) {
    switch (type) {
        case FT_REGULAR: // 普通文件
        case FT_DIRECTORY: // 目录
            return true;
        case FT_CHAR_SPECIAL:  // 字符设备 (不占块)
        case FT_BLOCK_SPECIAL: // 块设备文件本身 (不占块，它指向的是整个磁盘)
        case FT_PIPE: // 匿名管道 (不占块，不写回磁盘)
		case FT_FIFO: // 具名管道，不占磁盘数据块，但要inode要写回磁盘
        // case FT_SOCKET: // 套接字 (不占块)
            return false;
        default:
            return false;
    }
}

void inode_release(struct partition* part,uint32_t inode_no){
	struct m_inode* inode_to_del = inode_open(part,inode_no);
	ASSERT(inode_to_del->i_no==inode_no);

	// 如果是设备 inode，那么就不进行后续的释放操作，因为设备inode更笨
	if (!has_data_blocks(inode_to_del->di.i_type)){
		return;
	}

	uint8_t block_idx = 0,block_cnt = DIRECT_INDEX_BLOCK;
	uint32_t block_bitmap_idx;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};

	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = inode_to_del->di.i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(inode_to_del->di.i_sectors[tfflib]!=0){
		bread_multi(part->my_disk,inode_to_del->di.i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;

		block_bitmap_idx = inode_to_del->di.i_sectors[tfflib] - part->sb->data_start_lba;
		ASSERT(block_bitmap_idx>0);
		bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
		bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
	}

	block_idx = 0;
	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]!=0){
			block_bitmap_idx = 0;
			block_bitmap_idx = all_blocks_addr[block_idx]-part->sb->data_start_lba;
			ASSERT(block_bitmap_idx>0);
			bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
			bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
		}
		block_idx++;
	}

	bitmap_set(&part->inode_bitmap,inode_no,0);
	bitmap_sync(part,inode_no,INODE_BITMAP);

	void* io_buf = kmalloc(SECTOR_SIZE*2);
	inode_delete(part,inode_no,io_buf);
	kfree(io_buf);

	inode_close(inode_to_del);
}

// 创建匿名 inode
// 只在内存中创建，不去操作磁盘，主要是匿名 pipe 来用
struct m_inode* make_anonymous_inode() {
    struct m_inode* inode = (struct m_inode*)kmalloc(sizeof(struct m_inode));
    if (inode == NULL) return NULL;

    memset(inode, 0, sizeof(struct m_inode));

    // 核心身份标识
    inode->i_no = ANONY_I_NO; // 使用-1标志匿名inode
    inode->i_dev = -1; // 使用 -1 （全1）标志其没有存储设备
    inode->di.i_type = FT_PIPE;
    
    // 初始化引用计数：由 sys_pipe 进一步管理
    inode->i_open_cnts = 0; 

    // 注意！不要执行 dlist_pusb_back(&open_inodes, &inode->inode_tag) 
    // 匿名管道永远都不需要被搜索

    return inode;
}

// 从 inode 的 offset 处读取 count 字节到 buf
// 专门供 swap_page/惰性加载使用，不依赖 fd_table
int32_t inode_read_data(struct m_inode* inode, uint32_t offset, void* buf, uint32_t count) {
    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint8_t* buf_dst = (uint8_t*)buf;
    uint32_t size_left = count;

    // 边界检查：不能超过文件实际大小
    if (offset + count > inode->di.i_size) {
        size_left = inode->di.i_size - offset;
    }
    if (size_left <= 0) return 0;

    // 准备缓冲区和块地址表

    uint8_t* io_buf = kmalloc(BLOCK_SIZE);
    uint32_t* all_blocks_addr = (uint32_t*)kmalloc(TOTAL_BLOCK_COUNT * sizeof(uint32_t));
    if (!io_buf || !all_blocks_addr) {
        PANIC("inode_read_data: kmalloc failed");
    }
    memset(all_blocks_addr, 0, TOTAL_BLOCK_COUNT * sizeof(uint32_t));

    // 填充 block 地址表
    uint32_t block_start_idx = offset / BLOCK_SIZE;
    uint32_t block_end_idx = (offset + size_left - 1) / BLOCK_SIZE;

    // 填充直接块 (0-11)
    uint32_t idx = 0;
    while (idx < DIRECT_INDEX_BLOCK && idx <= block_end_idx) {
        all_blocks_addr[idx] = inode->di.i_sectors[idx];
        idx++;
    }

	// the first first-level index block
	uint32_t tfflib = DIRECT_INDEX_BLOCK;

    // 填充一级间接块 (12 及以后)
    if (block_end_idx >= tfflib) {
        ASSERT(inode->di.i_sectors[tfflib] != 0);
        // 从磁盘读取间接索引表到 all_blocks_addr 的后半部分
        bread_multi(part->my_disk, inode->di.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    // 开始搬运数据
    uint32_t bytes_read = 0;
    uint32_t curr_pos = offset;

    while (bytes_read < size_left) {
        uint32_t sec_idx = curr_pos / BLOCK_SIZE;
        uint32_t sec_lba = all_blocks_addr[sec_idx];
        
        uint32_t sec_off_bytes = curr_pos % BLOCK_SIZE;
        uint32_t sec_left_bytes = BLOCK_SIZE - sec_off_bytes;
        uint32_t chunk_size = (size_left - bytes_read < sec_left_bytes) ? 
                               (size_left - bytes_read) : sec_left_bytes;

        ASSERT(sec_lba != 0); // 正常文件（非空洞文件）不应为 0

        // 读取一个物理块
        bread_multi(part->my_disk, sec_lba, io_buf, 1);
        
        // 拷贝所需部分到目标缓冲区
        memcpy(buf_dst, io_buf + sec_off_bytes, chunk_size);

        buf_dst += chunk_size;
        curr_pos += chunk_size;
        bytes_read += chunk_size;
    }

    kfree(all_blocks_addr);
    kfree(io_buf);

    return bytes_read;
}