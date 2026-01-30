#include "dir.h"
#include "inode.h"
#include "ide.h"
#include "super_block.h"
#include "stdio-kernel.h"
#include "string.h"
#include "stdint.h"
#include "stdbool.h"
#include "debug.h"
#include "file.h"
#include "ide_buffer.h"

struct dir root_dir;

void open_root_dir(struct partition* part){
	root_dir.inode = inode_open(part,part->sb->root_inode_no);
	root_dir.dir_pos = 0;
}

void close_root_dir(struct partition* part){
	inode_close(root_dir.inode);
}

struct dir* dir_open(struct partition* part,uint32_t inode_no){
	struct dir* pdir = (struct dir*)kmalloc(sizeof(struct dir));
	pdir->inode = inode_open(part,inode_no);
	pdir->dir_pos = 0;
	return pdir;
}

bool search_dir_entry(struct partition* part,struct dir* pdir,const char* name,struct dir_entry* dir_e){
	// the block pointers in the inode are 32 bits wide
	// 32 bits = 4 bytes, so we divide by 4  
	uint32_t block_cnt = DIRECT_INDEX_BLOCK+FIRST_LEVEL_INDEX_BLOCK*(BLOCK_SIZE/ADDR_BYTES_32BIT);
	// alloc memory for all [block_cnt] addresses 
	uint32_t* all_blocks_addr = (uint32_t*)kmalloc(block_cnt*ADDR_BYTES_32BIT);
	if(all_blocks_addr==NULL){
		printk("in search_dir_entry: kmalloc for all_blocks failed!\n");
		return false;
	}

	uint32_t block_idx = 0;
	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = pdir->inode->i_sectors[block_idx];
		block_idx++;
	}

	block_idx = 0;
	// the first first-level index block
	uint32_t tfflib =  DIRECT_INDEX_BLOCK;
	if(pdir->inode->i_sectors[tfflib]!=0){
		bread_multi(part->my_disk,pdir->inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
	}


	uint8_t* buf = (uint8_t*)kmalloc(SECTOR_SIZE);
	struct dir_entry* p_de = (struct dir_entry*)buf;
	
	uint32_t dir_entry_size = part->sb->dir_entry_size;
	uint32_t dir_entry_cnt = SECTOR_SIZE/dir_entry_size;

	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]==0){
			block_idx++;
			continue;
		}
		bread_multi(part->my_disk,all_blocks_addr[block_idx],buf,1);

		uint32_t dir_entry_idx = 0;
		while(dir_entry_idx<dir_entry_cnt){
			if(!strcmp(p_de->filename,name)){
				memcpy(dir_e,p_de,dir_entry_size);
				kfree(buf);
				kfree(all_blocks_addr);
				return true;
			}
			dir_entry_idx++;
			p_de++;
		}
		block_idx++;
		p_de = (struct dir_entry*)buf;
		memset(buf,0,SECTOR_SIZE);
	}
	kfree(buf);
	kfree(all_blocks_addr);
	return false;
}

void dir_close(struct dir* dir){
	if(dir==&root_dir){
		return;
	}
	inode_close(dir->inode);
	kfree(dir);
}

void create_dir_entry(char* filename,uint32_t inode_no,uint8_t file_type,struct dir_entry* p_de){
	ASSERT(strlen(filename)<=MAX_FILE_NAME_LEN);

	memcpy(p_de->filename,filename,strlen(filename));
	p_de->i_no = inode_no;
	p_de->f_type = file_type;
}


bool sync_dir_entry(struct dir* parent_dir,struct dir_entry* p_de,void* io_buf){
	struct inode* dir_inode = parent_dir->inode;
	uint32_t dir_size = dir_inode->i_size;
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
	// the first first-level index block
	uint32_t tfflib =  DIRECT_INDEX_BLOCK;

	ASSERT(dir_size%dir_entry_size==0);

	uint32_t dir_entrys_per_sec = (SECTOR_SIZE/dir_entry_size);

	int32_t block_lba = -1;

	uint8_t block_idx = 0;
	
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};

	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}

	if (dir_inode->i_sectors[tfflib] != 0){
        bread_multi(cur_part->my_disk, dir_inode->i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

	struct dir_entry* dir_e = (struct dir_entry*)io_buf;

	int32_t block_bitmap_idx = -1;

	block_idx = 0;

	
	while(block_idx<TOTAL_BLOCK_COUNT){
		block_bitmap_idx = -1;
		if(all_blocks_addr[block_idx]==0){
			block_lba = block_bitmap_alloc(cur_part);
			if(block_lba==-1){
				printk("alloc block bitmap for sync_dir_entry failed!!!\n");
				return false;
			}

			if (cur_part == NULL) {
    			PANIC("cur_part is SMASHED!");
			}

			block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
			ASSERT(block_bitmap_idx!=-1);
			bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);
			
			block_bitmap_idx = -1;
			if(block_idx<tfflib){
				dir_inode->i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;
			}else if(block_idx==tfflib){
				dir_inode->i_sectors[tfflib] = block_lba;
				block_lba = -1;
				block_lba = block_bitmap_alloc(cur_part);

				if(block_lba==-1){
					block_bitmap_idx = dir_inode->i_sectors[tfflib]-cur_part->sb->data_start_lba;
					bitmap_set(&cur_part->block_bitmap,block_bitmap_idx,0);
					dir_inode->i_sectors[tfflib]=0;
					printk("alloc block bitmap for sync_dir_entry failed!!!\n");
					return false;
				}

				if (cur_part == NULL) {
   					PANIC("cur_part is SMASHED!");
				}

				block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;

				ASSERT(block_bitmap_idx!=-1);

				bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);

				all_blocks_addr[tfflib] = block_lba;
				bwrite_multi(cur_part->my_disk,dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
			}else{
				all_blocks_addr[block_idx] = block_lba;
				bwrite_multi(cur_part->my_disk,dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
			}

			memset(io_buf,0,SECTOR_SIZE);

			memcpy(io_buf,p_de,dir_entry_size);
			bwrite_multi(cur_part->my_disk,all_blocks_addr[block_idx],io_buf,1);
			dir_inode->i_size+=dir_entry_size;
			return true;
		}

		bread_multi(cur_part->my_disk,all_blocks_addr[block_idx],io_buf,1);

		uint8_t dir_entry_idx = 0;
		while(dir_entry_idx<dir_entrys_per_sec){
			if((dir_e+dir_entry_idx)->f_type==FT_UNKNOWN){
				memcpy(dir_e+dir_entry_idx,p_de,dir_entry_size);
				bwrite_multi(cur_part->my_disk,all_blocks_addr[block_idx],io_buf,1);

				dir_inode->i_size+=dir_entry_size;
				return true;
			}

			dir_entry_idx++;
		}
		block_idx++;
	}
	printk("directory is full!\n");
	return false;
}

bool delete_dir_entry(struct partition* part,struct dir* pdir,uint32_t inode_no,void* io_buf){
	struct inode* dir_inode = pdir->inode;
	uint32_t block_idx = 0,all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};
	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(dir_inode->i_sectors[tfflib]){
		bread_multi(part->my_disk,dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
	}

	uint32_t dir_entry_size = part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec = (SECTOR_SIZE/dir_entry_size);

	struct dir_entry* dir_e = (struct dir_entry*) io_buf;
	struct dir_entry* dir_entry_found = NULL;
	uint8_t dir_entry_idx,dir_entry_cnt;
	bool is_dir_first_block = false;

	block_idx = 0;
	while(block_idx<TOTAL_BLOCK_COUNT){
		is_dir_first_block = false;
		if(all_blocks_addr[block_idx]==0){
			block_idx++;
			continue;
		}
		dir_entry_idx = dir_entry_cnt = 0;
		memset(io_buf,0,SECTOR_SIZE);

		bread_multi(part->my_disk,all_blocks_addr[block_idx],io_buf,1);
		while(dir_entry_idx<dir_entrys_per_sec){
			if((dir_e+dir_entry_idx)->f_type!=FT_UNKNOWN){
				if(!strcmp((dir_e+dir_entry_idx)->filename,".")){
					is_dir_first_block = true;
				}else if(strcmp((dir_e+dir_entry_idx)->filename,".")&&strcmp((dir_e+dir_entry_idx)->filename,"..")){
					dir_entry_cnt++;
					if((dir_e+dir_entry_idx)->i_no==inode_no){
						ASSERT(dir_entry_found==NULL);
						dir_entry_found = dir_e+dir_entry_idx;
					}
				}
			}
			dir_entry_idx++;
		}

		if(dir_entry_found==NULL){
			block_idx++;
			continue;
		}

		ASSERT(dir_entry_cnt>=1);
		if(dir_entry_cnt==1&&!is_dir_first_block){
			uint32_t block_bitmap_idx = all_blocks_addr[block_idx]-part->sb->data_start_lba;
			bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
			bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);

			if(block_idx<DIRECT_INDEX_BLOCK){
				dir_inode->i_sectors[block_idx] = 0;
			}else{
				uint32_t indirect_blocks = 0;
				uint32_t indirect_block_idx = tfflib;
				while(indirect_block_idx<TOTAL_BLOCK_COUNT){
					if(all_blocks_addr[indirect_block_idx]!=0){
						indirect_blocks++;
					}
				}	
				ASSERT(indirect_blocks>=1);

				if(indirect_blocks>1){
					all_blocks_addr[block_idx] = 0;
					bwrite_multi(part->my_disk,dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
				}else{
					block_bitmap_idx = dir_inode->i_sectors[tfflib] - part->sb->data_start_lba;
					bitmap_set(&part->block_bitmap,block_bitmap_idx,0);
					bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);

					dir_inode->i_sectors[tfflib] = 0;
				}
			}
		}else{
			memset(dir_entry_found,0,dir_entry_size);
			bwrite_multi(part->my_disk,all_blocks_addr[block_idx],io_buf,1);
		}

		ASSERT(dir_inode->i_size>=dir_entry_size);
		dir_inode->i_size -= dir_entry_size;
		memset(io_buf,0,SECTOR_SIZE*2);
		inode_sync(part,dir_inode,io_buf);

		return true;
	}
	return false;
}
	
struct dir_entry* dir_read(struct dir* dir){
	struct dir_entry* dir_e = (struct dir_entry*)dir->dir_buf;
	struct inode* dir_inode = dir->inode;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0},block_cnt = DIRECT_INDEX_BLOCK;
	uint32_t block_idx = 0,dir_entry_idx = 0;
	
	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = dir_inode->i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(dir_inode->i_sectors[tfflib]!=0){
		bread_multi(cur_part->my_disk,dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;
	}
	block_idx = 0;

	uint32_t cur_dir_entry_pos = 0;
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec = SECTOR_SIZE / dir_entry_size;
	while(dir->dir_pos<dir_inode->i_size){
		if(dir->dir_pos>=dir_inode->i_size){
			return NULL;
		}
		if(all_blocks_addr[block_idx]==0){
			block_idx++;
			continue;
		}
		memset(dir_e,0,SECTOR_SIZE);
		bread_multi(cur_part->my_disk,all_blocks_addr[block_idx],dir_e,1);
		dir_entry_idx = 0;
		while(dir_entry_idx<dir_entrys_per_sec){
			if((dir_e+dir_entry_idx)->f_type!=FT_UNKNOWN){
				// printk("dir_pos: %d\n",dir->dir_pos);
				// printk("i_size: %d\n",dir->inode->i_size);
				if(cur_dir_entry_pos<dir->dir_pos){
					cur_dir_entry_pos += dir_entry_size;
					dir_entry_idx++;
					continue;
				}
				ASSERT(cur_dir_entry_pos==dir->dir_pos);
				dir->dir_pos += dir_entry_size;
				return dir_e+dir_entry_idx;
			}
			dir_entry_idx++;
		}
		block_idx++;
	}
	return NULL;
}


bool dir_is_empty(struct dir* dir){
	// if the directory only has two entry "." and ".."
	// then it is empty 
	struct inode* dir_inode =dir->inode;
	return (dir_inode->i_size==cur_part->sb->dir_entry_size*2);
}

int32_t dir_remove(struct dir* parent_dir,struct dir* child_dir){
	struct inode* child_dir_inode = child_dir->inode;
	int32_t block_idx = 1;
	while(block_idx<13){
		ASSERT(child_dir_inode->i_sectors[block_idx]==0);
		block_idx++;
	}
	void* io_buf = kmalloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		printk("dir_remove: malloc for io_buf failed!\n");
		return -1;
	}

	delete_dir_entry(cur_part,parent_dir,child_dir_inode->i_no,io_buf);

	inode_release(cur_part,child_dir_inode->i_no);
	kfree(io_buf);
	return 0;
}