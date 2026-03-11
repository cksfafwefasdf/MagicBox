#include <sifs_file.h>
#include <stdint.h>
#include <string.h>
#include <stdio-kernel.h>
#include <thread.h>
#include <ide.h>
#include <ide_buffer.h>
#include <fs.h>
#include <debug.h>
#include <sifs_inode.h>
#include <sifs_dir.h>
#include <interrupt.h>
#include <process.h>
#include <file_table.h>
#include <sifs_sb.h>
#include <inode.h>
#include <errno.h>

int32_t sifs_file_write(struct file* file,const void* buf,uint32_t count){

	struct partition* part = get_part_by_rdev(file->fd_inode->i_dev);

	if((file->fd_inode->i_size+count)>(BLOCK_SIZE*TOTAL_BLOCK_COUNT)){
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

	if(file->fd_inode->sifs_i.i_sectors[0]==0){
		block_lba = block_bitmap_alloc(part);
		if(block_lba==-1){
			printk("file_write: block_bitmap_alloc failed!\n");
			return -1;
		}
		file->fd_inode->sifs_i.i_sectors[0] = block_lba;

		block_bitmap_idx = block_lba - part->sb->sifs_info.sb_raw.data_start_lba;
		ASSERT(block_bitmap_idx!=0);
		bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
	}

	uint32_t file_has_used_blocks = file->fd_inode->i_size/BLOCK_SIZE+1;

	uint32_t file_will_use_blocks = (file->fd_inode->i_size+count)/BLOCK_SIZE+1;
	ASSERT(file_will_use_blocks<=TOTAL_BLOCK_COUNT);

	uint32_t addr_blocks = file_will_use_blocks - file_has_used_blocks;

	if(addr_blocks==0){
		if(file_will_use_blocks<=DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks-1;
			all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];
		}else{
			// the first first-level index block
			uint32_t tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
			partition_read(part,indirect_block_table,all_blocks_addr+tfflib,1);
		}
	}else{
		if(file_will_use_blocks<=DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks - 1;
			ASSERT(file->fd_inode->sifs_i.i_sectors[block_idx]!=0);
			all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];

			block_idx = file_has_used_blocks;
			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap_alloc for situation 1 failed!\n");
					return -1;
				}
				ASSERT(file->fd_inode->sifs_i.i_sectors[block_idx]==0);

				file->fd_inode->sifs_i.i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;

				block_bitmap_idx = block_lba - part->sb->sifs_info.sb_raw.data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);

				block_idx++;
			}
		}else if(file_has_used_blocks<=DIRECT_INDEX_BLOCK&&file_will_use_blocks>DIRECT_INDEX_BLOCK){
			block_idx = file_has_used_blocks - 1;
			all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];
			
			block_lba = block_bitmap_alloc(part);
			if(block_lba==-1){
				printk("file_write: block_bitmap_alloc for situation 2 failed\n");
				return -1;
			}
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]==0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib] = block_lba;

			block_idx = file_has_used_blocks;

			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap__alloc for situation 2 failed!\n");
					return -1;
				}
				if(block_idx<DIRECT_INDEX_BLOCK){
					ASSERT(file->fd_inode->sifs_i.i_sectors[block_idx]==0);

					file->fd_inode->sifs_i.i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;
				}else{
					all_blocks_addr[block_idx] = block_lba;
				}
				block_bitmap_idx = block_lba-part->sb->sifs_info.sb_raw.data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);

				block_idx++;
			}
			partition_write(part,indirect_block_table,all_blocks_addr+12,1);
		}else if(file_has_used_blocks>DIRECT_INDEX_BLOCK){
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];

			partition_read(part,indirect_block_table,all_blocks_addr+12,1);

			block_idx = file_has_used_blocks;

			while(block_idx<file_will_use_blocks){
				block_lba = block_bitmap_alloc(part);
				if(block_lba==-1){
					printk("file_write: block_bitmap_alloc or situation 3 failed\n");
					return -1;
				}
				all_blocks_addr[block_idx++] = block_lba;

				block_bitmap_idx = block_lba - part->sb->sifs_info.sb_raw.data_start_lba;
				bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
			}
			partition_write(part,indirect_block_table,all_blocks_addr+12,1);
		}
	}
	// To mark the last block containing valid data with free space
	bool last_valid_block_with_space = true;

	file->fd_pos = file->fd_inode->i_size-1;

	while(bytes_written<count){
		memset(io_buf,0,BLOCK_SIZE);
		sec_idx = file->fd_inode->i_size/BLOCK_SIZE;
		sec_lba = all_blocks_addr[sec_idx];
		sec_off_bytes = file->fd_inode->i_size% BLOCK_SIZE;
		sec_left_bytes = BLOCK_SIZE - sec_off_bytes;

		chunk_size = size_left<sec_left_bytes?size_left:sec_left_bytes;
		if(last_valid_block_with_space){
			partition_read(part,sec_lba,io_buf,1);
			last_valid_block_with_space = false;
		}
		memcpy(io_buf+sec_off_bytes,src,chunk_size);
		partition_write(part,sec_lba,io_buf,1);
		printk("file write at lba 0x%x\n",sec_lba);
		
		src += chunk_size;
		file->fd_inode->i_size += chunk_size;
		file->fd_pos +=chunk_size;
		bytes_written +=chunk_size; 
		size_left-=chunk_size;
	}
	// printk("file_create:::part:%x dir_inode:%x\n",part->i_rdev,file->fd_inode->i_dev);
	file->fd_inode->i_sb->s_op->write_inode(file->fd_inode);
	kfree(all_blocks_addr);
	kfree(io_buf);
	return bytes_written;
}

int32_t sifs_file_read(struct file* file,void* buf,uint32_t count){
	
	struct partition* part = get_part_by_rdev(file->fd_inode->i_dev);

	uint8_t* buf_dst = (uint8_t*) buf;
	uint32_t size = count;
	uint32_t size_left = size;
	if((file->fd_pos+count)>file->fd_inode->i_size){
		size = file->fd_inode->i_size - file->fd_pos;
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
			all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];
		}else{
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[12];
			partition_read(part,indirect_block_table,all_blocks_addr+12,1);
		}
	}else{
		if(block_read_end_idx<DIRECT_INDEX_BLOCK){
			block_idx = block_read_start_idx;
			while(block_idx<=block_read_end_idx){
				all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];
				block_idx++;
			}
		}else if(block_read_start_idx<DIRECT_INDEX_BLOCK&&block_read_end_idx>=DIRECT_INDEX_BLOCK){
			block_idx=block_read_start_idx;
			while(block_idx<DIRECT_INDEX_BLOCK){
				all_blocks_addr[block_idx] = file->fd_inode->sifs_i.i_sectors[block_idx];
				block_idx++;
			}
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);

			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
			partition_read(part,indirect_block_table,all_blocks_addr+tfflib,1);

		}else{
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
			partition_read(part,indirect_block_table,all_blocks_addr+tfflib,1);
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
		
		partition_read(part,sec_lba,io_buf,1);

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

