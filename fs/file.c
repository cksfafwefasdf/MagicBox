#include "file.h"
#include "stdint.h"
#include "string.h"
#include "stdio-kernel.h"
#include "thread.h"
#include "ide.h"
#include "ide_buffer.h"
#include "fs.h"
#include "debug.h"
#include "inode.h"
#include "dir.h"
#include "interrupt.h"
#include "process.h"


// System-wide Open File Table
struct file file_table[MAX_FILE_OPEN_IN_SYSTEM];


int32_t get_free_slot_in_global(void){
	uint32_t fd_idx = 3;
	while(fd_idx<MAX_FILE_OPEN_IN_SYSTEM){
		if(file_table[fd_idx].fd_inode==NULL){
			// 找到了就地清空，防止残留脏数据
            memset(&file_table[fd_idx], 0, sizeof(struct file)); 
            break;
		}
		fd_idx++;
	}
	if(fd_idx==MAX_FILE_OPEN_IN_SYSTEM){
		printk("exceed max open files!\n");
		return -1;
	}
	return fd_idx;
}

int32_t pcb_fd_install(int32_t global_fd_idx){
	struct task_struct* cur = get_running_task_struct();

	uint8_t local_fd_idx = 0;
	while(local_fd_idx<MAX_FILES_OPEN_PER_PROC){
		if(cur->fd_table[local_fd_idx]==-1){
			cur->fd_table[local_fd_idx]=global_fd_idx;
			break;
		}
		local_fd_idx++;
	}

	if(local_fd_idx==MAX_FILES_OPEN_PER_PROC){
		printk("exceed max open file_per_proc!\n");
		return -1;
	}
	return local_fd_idx;
}

int32_t inode_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->inode_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->inode_bitmap,bit_idx,1);
	return bit_idx;
}

int32_t block_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->block_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->block_bitmap,bit_idx,1);
	return (part->sb->data_start_lba+bit_idx);
}

// write the sector containing the bit_idx of the in-memory bitmap back to disk
void bitmap_sync(struct partition* part,uint32_t bit_idx,enum bitmap_type btmp_type){
	// Since bit_idx is in bits, we first convert it to bytes by dividing by 8
	uint32_t off_sec = bit_idx/(8*SECTOR_SIZE); 

	uint32_t off_size = off_sec*SECTOR_SIZE;

	uint32_t sec_lba;
	uint8_t* bitmap_off;

	switch (btmp_type){
		case INODE_BITMAP:
			sec_lba = part->sb->inode_bitmap_lba+off_sec;
			bitmap_off = part->inode_bitmap.bits+off_size;
			break;
		case BLOCK_BITMAP:
			sec_lba = part->sb->block_bitmap_lba+off_sec;
			bitmap_off = part->block_bitmap.bits+off_size;
			break;
		default:
			PANIC("unknown bitmap type!!!\n");
			return;
	}
	bwrite_multi(part->my_disk,sec_lba,bitmap_off,1);
}

// create a regular file
int32_t file_create(struct dir* parent_dir,char* filename,uint8_t flag){
	void* io_buf = kmalloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		printk("in file_create: kmalloc for io_buf failed!\n");
		return -1;
	}

	struct partition* part = get_part_by_rdev(parent_dir->inode->i_dev);

	uint8_t rollback_step = 0;

	int32_t inode_no = inode_bitmap_alloc(part);
	if(inode_no==-1){
		printk("in file_create: allocate inode failed!\n");
		return -1;
	}
	
	// malloc space for inode
	// this space should be in the kernel heap space
	
	struct m_inode* new_file_inode = (struct m_inode*)kmalloc(sizeof(struct m_inode));
	

	if(new_file_inode==NULL){
		printk("in file_create: kmalloc for inode failed!\n");
		rollback_step = 1;
		goto rollback;
	}

	inode_init(part,inode_no,new_file_inode,FT_REGULAR);
	
	// printk("file_create:::new_file_inode addr: %x\n",new_file_inode);
	int fd_idx = get_free_slot_in_global();
	if(fd_idx==-1){
		printk("exceed max open files!\n");
		rollback_step = 2;
		goto rollback;
	}

	file_table[fd_idx].fd_inode = new_file_inode;
	file_table[fd_idx].fd_pos = 0;
	file_table[fd_idx].fd_flag = flag;
	file_table[fd_idx].fd_inode->write_deny = false;
	file_table[fd_idx].f_count = 1;

	struct dir_entry new_dir_entry;
	memset(&new_dir_entry,0,sizeof(struct dir_entry));

	create_dir_entry(filename,inode_no,FT_REGULAR,&new_dir_entry);

	if(!sync_dir_entry(parent_dir,&new_dir_entry,io_buf)){
		printk("sync dir_entry to disk failed!\n");
		rollback_step = 3;
		goto rollback;
	}
	memset(io_buf,0,SECTOR_SIZE*2);
	// printk("file_create1:::part:%x dir_inode:%x\n",part->i_rdev,parent_dir->inode->i_dev);
	inode_sync(part,parent_dir->inode,io_buf);
	memset(io_buf,0,SECTOR_SIZE*2);
	// printk("file_create2:::part:%x dir_inode:%x\n",part->i_rdev,new_file_inode->i_dev);
	inode_sync(part,new_file_inode,io_buf);
	bitmap_sync(part,inode_no,INODE_BITMAP);

	dlist_push_front(&part->open_inodes,&new_file_inode->inode_tag);
	new_file_inode->i_open_cnts = 1;

	kfree(io_buf);
	return pcb_fd_install(fd_idx);

rollback:
	switch (rollback_step){
		case 3:
			memset(&file_table[fd_idx],0,sizeof(struct file));
		case 2:
			kfree(new_file_inode);
		case 1:
			bitmap_set(&part->inode_bitmap,inode_no,0);
			break;
	}
	kfree(io_buf);
	return -1;
}

int32_t file_open(struct partition* part, uint32_t inode_no,uint8_t flag){
	int32_t fd_idx = get_free_slot_in_global();
	if(fd_idx==-1){
		printk("exceed max open files\n");
		return -1;
	} 
	// file_open 将一个全局描述符绑定到了一个inode上
	// 因此 i_open_cnt 需要加一，这个加一在 inode_open 中进行
	file_table[fd_idx].fd_inode = inode_open(part,inode_no);
	file_table[fd_idx].fd_pos = 0;
	file_table[fd_idx].f_count = 1;

	file_table[fd_idx].fd_flag = flag;
	bool *write_deny = &(file_table[fd_idx].fd_inode->write_deny);

	if(flag&O_WRONLY||flag&O_RDWR){
		enum intr_status old_status = intr_disable();
		if(!(*write_deny)){
			*write_deny = true;
			intr_set_status(old_status);
		}else{
			intr_set_status(old_status);
			printk("file can't be write now,try again later!\n");
			return -1;
		}
	}
	return pcb_fd_install(fd_idx);
}

// if success, then return 0
// else return -1
int32_t file_close(struct file* file){
	if(file==NULL){
		return -1;
	}

	file->f_count--;
	// 只有当这是最后一个指向该 inode 的 file 时才真正释放保护
	if (file->fd_inode->i_open_cnts == 1) {
        file->fd_inode->write_deny = false;
    }

	// 只有当 file 的 f_count 为 0 时
	// 说明这个全局表项可以清空了
	// 因此，改该全局表项对应的 inode 的打开数量也要减1
	// 这个打开计数是在 inode_close 中减少的
	if (file->f_count == 0) {
        // 這時才真正去減少 Inode 的計數
        // 因為這個“打開文件句柄”已經徹底沒人用了
        inode_close(file->fd_inode); 
        
        // 此時將指針置空是安全的，因為沒有 FD 指向這裡了
        file->fd_inode = NULL; 
    }
	return 0;
}

int32_t file_write(struct file* file,const void* buf,uint32_t count){

	struct partition* part = get_part_by_rdev(file->fd_inode->i_dev);

	if((file->fd_inode->di.i_size+count)>(BLOCK_SIZE*TOTAL_BLOCK_COUNT)){
		printk("exceed max file_size %d bytes, write file failed!\n",BLOCK_SIZE*TOTAL_BLOCK_COUNT);
		return -1;
	}
	// since the file may span 2 sectors
	// we need allocate SECTOR_SIZE*2 bytes for io_buf
	uint8_t* io_buf = kmalloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		printk("file_write: kmalloc for io_buf failed!\n");
		return -1;
	}
	uint32_t* all_blocks_addr = (uint32_t*) kmalloc(TOTAL_BLOCK_COUNT*ADDR_BYTES_32BIT);
	if(all_blocks_addr==NULL){
		printk("file_write: kmalloc for all blocks failed!\n");
		return -1;
	}

	const uint8_t* src = buf;
	uint32_t bytes_written = 0;
	uint32_t size_left = count;
	int32_t block_lba = -1;
	uint32_t block_bitmap_idx = 0;

	uint32_t sec_idx;
	uint32_t sec_lba;
	uint32_t sec_off_bytes;
	uint32_t sec_left_bytes;
	uint32_t chunk_size;
	int32_t indirect_block_table;
	uint32_t block_idx;

	if(file->fd_inode->di.i_sectors[0]==0){
		block_lba = block_bitmap_alloc(part);
		if(block_lba==-1){
			printk("file_write: block_bitmap_alloc failed!\n");
			return -1;
		}
		file->fd_inode->di.i_sectors[0] = block_lba;

		block_bitmap_idx = block_lba - part->sb->data_start_lba;
		ASSERT(block_bitmap_idx!=0);
		bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
	}

	uint32_t file_has_used_blocks = file->fd_inode->di.i_size/BLOCK_SIZE+1;

	uint32_t file_will_use_blocks = (file->fd_inode->di.i_size+count)/BLOCK_SIZE+1;
	ASSERT(file_will_use_blocks<=TOTAL_BLOCK_COUNT);

	uint32_t addr_blocks = file_will_use_blocks - file_has_used_blocks;

	if(addr_blocks==0){
		if(file_will_use_blocks<=DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks-1;
			all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];
		}else{
			// the first first-level index block
			uint32_t tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->di.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->di.i_sectors[tfflib];
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+tfflib,1);
		}
	}else{
		if(file_will_use_blocks<=DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks - 1;
			ASSERT(file->fd_inode->di.i_sectors[block_idx]!=0);
			all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];

			block_idx = file_has_used_blocks;
			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap_alloc for situation 1 failed!\n");
					return -1;
				}
				ASSERT(file->fd_inode->di.i_sectors[block_idx]==0);

				file->fd_inode->di.i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;

				block_bitmap_idx = block_lba - part->sb->data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);

				block_idx++;
			}
		}else if(file_has_used_blocks<=DIRECT_INDEX_BLOCK&&file_will_use_blocks>DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks - 1;
			all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];
			
			block_lba = block_bitmap_alloc(part);
			if(block_lba==-1){
				printk("file_write: block_bitmap_alloc for situation 2 failed\n");
				return -1;
			}
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->di.i_sectors[tfflib]==0);
			indirect_block_table = file->fd_inode->di.i_sectors[tfflib] = block_lba;

			block_idx = file_has_used_blocks;

			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap__alloc for situation 2 failed!\n");
					return -1;
				}
				if(block_idx<DIRECT_INDEX_BLOCK){
					ASSERT(file->fd_inode->di.i_sectors[block_idx]==0);

					file->fd_inode->di.i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;
				}else{
					all_blocks_addr[block_idx] = block_lba;
				}
				block_bitmap_idx = block_lba-part->sb->data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);

				block_idx++;
			}
			bwrite_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
		}else if(file_has_used_blocks>DIRECT_INDEX_BLOCK){
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->di.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->di.i_sectors[tfflib];

			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);

			block_idx = file_has_used_blocks;

			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap_alloc or situation 3 failed\n");
					return -1;
				}
				all_blocks_addr[block_idx++] = block_lba;

				block_bitmap_idx = block_lba - part->sb->data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
			}
			bwrite_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
		}
	}
	// To mark the last block containing valid data with free space
	bool last_valid_block_with_space = true;

	file->fd_pos = file->fd_inode->di.i_size-1;

	while(bytes_written<count){
		memset(io_buf,0,BLOCK_SIZE);
		sec_idx = file->fd_inode->di.i_size/BLOCK_SIZE;
		sec_lba = all_blocks_addr[sec_idx];
		sec_off_bytes = file->fd_inode->di.i_size% BLOCK_SIZE;
		sec_left_bytes = BLOCK_SIZE - sec_off_bytes;

		chunk_size = size_left<sec_left_bytes?size_left:sec_left_bytes;
		if(last_valid_block_with_space){
			bread_multi(part->my_disk,sec_lba,io_buf,1);
			last_valid_block_with_space = false;
		}
		memcpy(io_buf+sec_off_bytes,src,chunk_size);
		bwrite_multi(part->my_disk,sec_lba,io_buf,1);
		printk("file write at lba 0x%x\n",sec_lba);
		
		src += chunk_size;
		file->fd_inode->di.i_size += chunk_size;
		file->fd_pos +=chunk_size;
		bytes_written +=chunk_size; 
		size_left-=chunk_size;
	}
	// printk("file_create:::part:%x dir_inode:%x\n",part->i_rdev,file->fd_inode->i_dev);
	inode_sync(part,file->fd_inode,io_buf);
	kfree(all_blocks_addr);
	kfree(io_buf);
	return bytes_written;
}

int32_t file_read(struct file* file,void* buf,uint32_t count){
	
	struct partition* part = get_part_by_rdev(file->fd_inode->i_dev);

	uint8_t* buf_dst = (uint8_t*) buf;
	uint32_t size = count;
	uint32_t size_left = size;
	if((file->fd_pos+count)>file->fd_inode->di.i_size){
		size = file->fd_inode->di.i_size - file->fd_pos;
		size_left = size;
		if(size==0){
			return -1;
		}
	}
	uint8_t *io_buf = kmalloc(BLOCK_SIZE);
	
	if(io_buf==NULL){
		printk("file_read: kmalloc for io_buf failed!\n");
		return -1;
	} 
	
	uint32_t* all_blocks_addr = (uint32_t*)kmalloc(TOTAL_BLOCK_COUNT*ADDR_BYTES_32BIT);

	// printk("file_read:::all_blocks_addr addr: %x\n",all_blocks_addr);
	
	if(all_blocks_addr==NULL){
		printk("file_read: kmalloc for all_blocks_addr failed!\n");
		return -1;
	}

	uint32_t block_read_start_idx = file->fd_pos/BLOCK_SIZE;
	uint32_t block_read_end_idx = (file->fd_pos+size)/BLOCK_SIZE;
	uint32_t read_blocks = block_read_start_idx-block_read_end_idx;

	ASSERT(block_read_start_idx<TOTAL_BLOCK_COUNT-1&&block_read_end_idx<TOTAL_BLOCK_COUNT-1);

	int32_t indirect_block_table;
	uint32_t block_idx;
	

	if(read_blocks==0){
		ASSERT(block_read_end_idx==block_read_start_idx);
		if(block_read_end_idx<DIRECT_INDEX_BLOCK){
			block_idx = block_read_end_idx;
			all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];
		}else{
			indirect_block_table = file->fd_inode->di.i_sectors[12];
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
		}
	}else{
		if(block_read_end_idx<DIRECT_INDEX_BLOCK){
			block_idx = block_read_start_idx;
			while(block_idx<=block_read_end_idx){
				all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];
				block_idx++;
			}
		}else if(block_read_start_idx<DIRECT_INDEX_BLOCK&&block_read_end_idx>=DIRECT_INDEX_BLOCK){
			block_idx=block_read_start_idx;
			while(block_idx<DIRECT_INDEX_BLOCK){
				all_blocks_addr[block_idx] = file->fd_inode->di.i_sectors[block_idx];
				block_idx++;
			}
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->di.i_sectors[tfflib]!=0);

			indirect_block_table = file->fd_inode->di.i_sectors[tfflib];
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+tfflib,1);

		}else{
						// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->di.i_sectors[tfflib]!=0);
			ASSERT(file->fd_inode->di.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->di.i_sectors[tfflib];
			indirect_block_table = file->fd_inode->di.i_sectors[tfflib];
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+tfflib,1);
		}
	}

	uint32_t sec_idx,sec_lba,sec_off_bytes,sec_left_bytes,chunk_size;
	uint32_t bytes_read = 0;
	while(bytes_read<size){
		sec_idx = file->fd_pos/BLOCK_SIZE;
		sec_lba = all_blocks_addr[sec_idx];
		sec_off_bytes = file->fd_pos%BLOCK_SIZE;
		sec_left_bytes = BLOCK_SIZE-sec_off_bytes;
		chunk_size = size_left<sec_left_bytes?size_left:sec_left_bytes;
		ASSERT(sec_idx < TOTAL_BLOCK_COUNT);
		memset(io_buf,0,BLOCK_SIZE);
		
		bread_multi(part->my_disk,sec_lba,io_buf,1);

		memcpy(buf_dst,io_buf+sec_off_bytes,chunk_size);
		buf_dst+=chunk_size;
		file->fd_pos+=chunk_size;
		bytes_read+=chunk_size;
		size_left-=chunk_size;
	}

	
	kfree(all_blocks_addr);
	kfree(io_buf);

	return bytes_read;
}