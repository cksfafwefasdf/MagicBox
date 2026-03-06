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

int32_t inode_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->sb->sifs_info.inode_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->sb->sifs_info.inode_bitmap,bit_idx,1);
	return bit_idx;
}

int32_t block_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->sb->sifs_info.block_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->sb->sifs_info.block_bitmap,bit_idx,1);
	return (part->sb->sifs_info.sb_raw.data_start_lba+bit_idx);
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
			sec_lba = part->sb->sifs_info.sb_raw.inode_bitmap_lba+off_sec;
			bitmap_off = part->sb->sifs_info.inode_bitmap.bits+off_size;
			break;
		case BLOCK_BITMAP:
			sec_lba = part->sb->sifs_info.sb_raw.block_bitmap_lba+off_sec;
			bitmap_off = part->sb->sifs_info.block_bitmap.bits+off_size;
			break;
		default:
			PANIC("unknown bitmap type!!!\n");
			return;
	}
	bwrite_multi(part->my_disk,sec_lba,bitmap_off,1);
}

// create a regular file
int32_t sifs_file_create(struct inode* parent_inode, char* filename, uint8_t flag) {
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) {
        printk("in sifs_file_create: kmalloc for io_buf failed!\n");
        return -1;
    }

    struct partition* part = get_part_by_rdev(parent_inode->i_dev);
    uint8_t rollback_step = 0;

    // 分配 Inode 编号
    int32_t inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        printk("in sifs_file_create: allocate inode failed!\n");
        kfree(io_buf);
        return -1;
    }

    // 为新 Inode 分配内核堆内存
    struct inode* new_file_inode = (struct inode*)kmalloc(sizeof(struct inode));
    if (new_file_inode == NULL) {
        printk("in sifs_file_create: kmalloc for inode failed!\n");
        rollback_step = 1;
        goto rollback;
    }

    // 初始化 Inode (设置类型、大小、块索引等)
    inode_init(part, inode_no, new_file_inode, FT_REGULAR);
    
    // 在全局文件表中获取空位
    int fd_idx = get_free_slot_in_global();
    if (fd_idx == -1) {
        printk("exceed max open files!\n");
        rollback_step = 2;
        goto rollback;
    }

    // 填充全局 file 结构
    file_table[fd_idx].fd_inode = new_file_inode;
    file_table[fd_idx].fd_pos = 0;
    file_table[fd_idx].fd_flag = flag;
    file_table[fd_idx].f_count = 1;
    file_table[fd_idx].fd_inode->write_deny = false;

    // 在父目录中创建目录项
    struct sifs_dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct sifs_dir_entry));
    sifs_create_dir_entry(filename, inode_no, FT_REGULAR, &new_dir_entry);

    // 使用 parent_inode 
    if (!sifs_sync_dir_entry(parent_inode, &new_dir_entry, io_buf)) {
        printk("sync dir_entry to disk failed!\n");
        rollback_step = 3;
        goto rollback;
    }

    // 同步元数据到磁盘
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, parent_inode, io_buf); // 同步父目录更新后的 i_size
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, new_file_inode, io_buf); // 同步新文件的 Inode
    bitmap_sync(part, inode_no, INODE_BITMAP); // 同步 Inode 位图
	

    // 将新 Inode 加入分区的 open_inodes 链表
    // 这样以后其他进程 open 这个文件时，就能从内存链表中找到，保证唯一性
	new_file_inode->i_open_cnts = 1;
	inode_register_to_cache(new_file_inode);
	
    kfree(io_buf);
    return pcb_fd_install(fd_idx); // 安装到进程的 FD 表中

rollback:
    switch (rollback_step) {
        case 3:
            memset(&file_table[fd_idx], 0, sizeof(struct file));
        case 2:
            kfree(new_file_inode);
        case 1:
            bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
            break;
    }
    kfree(io_buf);
    return -1;
}

// if success, then return 0
// else return -1
int32_t file_close(struct file* file){
	if (file == NULL || file->fd_inode == NULL) {
        return -1;
    }

	file->f_count--;
	// 只有当这个文件是以“独占写”或“执行”模式打开，且我们要彻底释放它时才恢复
	if (file->fd_inode->i_open_cnts == 1 && file->f_count == 1) {
        file->fd_inode->write_deny = false;
    }

	// 只有当 file 的 f_count 为 0 时
	// 说明这个全局表项可以清空了
	// 因此，改该全局表项对应的 inode 的打开数量也要减1
	// 这个打开计数是在 inode_close 中减少的
	if (file->f_count == 0) {

		// 如果是以写权限打开的文件，需要将write_deny置为false
		// 否则缓存上的inode他的write_deny会一直为true，下次缓存命中时，即使没有进程在写
		// 其他进程也不能写！必须要判断当前的这个file是否是在写才行
		// 否则会出现一个进程在写，另一个进程在读，这个读进程退出后直接将这个写进程的write_deny置为false的清空
		// 其实更好的办法应该是要将其换成写计数而不是一个bool的写保护
		// 不然难以处理多进程写的问题
		if((file->fd_flag&O_RDWR)||(file->fd_flag&O_WRONLY)){
			file->fd_inode->write_deny = false;
		}

        // 这时才真正去減少 Inode 的计数
        // 因为这个“打开文件句柄”已经彻底沒人用了
        inode_close(file->fd_inode); 
        
        // 此时将指針置空是安全的，因为没有 FD 指向这里了
        file->fd_inode = NULL; 
        file->fd_pos = 0;
        file->fops = NULL;
        file->fd_flag = 0;
    }
	return 0;
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

int32_t file_write(struct file* file,const void* buf,uint32_t count){

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
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+tfflib,1);
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
			bwrite_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
		}else if(file_has_used_blocks>DIRECT_INDEX_BLOCK){
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];

			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);

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
			bwrite_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
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
			bread_multi(part->my_disk,sec_lba,io_buf,1);
			last_valid_block_with_space = false;
		}
		memcpy(io_buf+sec_off_bytes,src,chunk_size);
		bwrite_multi(part->my_disk,sec_lba,io_buf,1);
		printk("file write at lba 0x%x\n",sec_lba);
		
		src += chunk_size;
		file->fd_inode->i_size += chunk_size;
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
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+12,1);
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
			bread_multi(part->my_disk,indirect_block_table,all_blocks_addr+tfflib,1);

		}else{
			// the first first-level index block
			int tfflib = DIRECT_INDEX_BLOCK;
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			ASSERT(file->fd_inode->sifs_i.i_sectors[tfflib]!=0);
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
			indirect_block_table = file->fd_inode->sifs_i.i_sectors[tfflib];
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