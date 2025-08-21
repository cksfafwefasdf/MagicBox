#include "fs.h"
#include "inode.h"
#include "super_block.h"
#include "dir.h"
#include "debug.h"
#include "string.h"
#include "ide.h"
#include "stdio-kernel.h"
#include "stdbool.h"
#include "stdint.h"
#include "dlist.h"
#include "file.h"
#include "console.h"
#include "thread.h"
#include "ioqueue.h"
#include "keyboard.h"
#include "pipe.h"
#include "bitmap.h"
#include "memory.h"


struct partition* cur_part;
struct dir* cur_dir;

// logic formatlize 
static void partition_format(struct partition* part){
	uint32_t boot_sector_sects = 1;
	uint32_t super_block_sects = 1;
	uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART,BITS_PER_SECTOR);
	uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct inode)*MAX_FILES_PER_PART)),SECTOR_SIZE);
	uint32_t used_sects = boot_sector_sects+super_block_sects+inode_bitmap_sects+inode_table_sects;
	uint32_t free_sects = part->sec_cnt - used_sects;

	uint32_t block_bitmap_sects;
	block_bitmap_sects = DIV_ROUND_UP(free_sects,BITS_PER_SECTOR);
	uint32_t block_bitmap_bit_len = free_sects-block_bitmap_sects;
	block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len,BITS_PER_SECTOR);

	struct super_block sb;
	sb.magic = FS_MAGIC_NUMBER;
	sb.sec_cnt = part->sec_cnt;
	sb.inode_cnt = MAX_FILES_PER_PART;
	sb.part_lba_base = part->start_lba;

	sb.block_bitmap_lba = sb.part_lba_base+2;
	sb.block_bitmap_sects = block_bitmap_sects;

	sb.inode_bitmap_lba = sb.block_bitmap_lba+sb.block_bitmap_sects;
	sb.inode_bitmap_sects = inode_bitmap_sects;

	sb.inode_table_lba = sb.inode_bitmap_lba+sb.inode_bitmap_sects;
	sb.inode_table_sects = inode_table_sects;

	sb.data_start_lba = sb.inode_table_lba+sb.inode_table_sects;
	sb.root_inode_no = 0;
	sb.dir_entry_size = sizeof(struct dir_entry);

	printk("%s info:\n",part->name);
	printk("\tmagic:0x%x\n\
\tpart_lba_base:0x%x\n\
\tall_sectors:0x%x\n\
\tinode_cnt:0x%x\n\
\tblock_bitmap_lba:0x%x\n\
\tblock_bitmap_sectors:0x%x\n\
\tinode_bitmap_lba:0x%x\n\
\tinode_bitmap_sectors:0x%x\n\
\tinode_table_lba:0x%x\n\
\tinode_table_sectors:0x%x\n\
\tdata_start_lba:0x%x\n", 
	sb.magic,
	sb.part_lba_base,
	sb.sec_cnt,
	sb.inode_cnt,
	sb.block_bitmap_lba,sb.block_bitmap_sects,
	sb.inode_bitmap_lba,sb.inode_bitmap_sects,
	sb.inode_table_lba,sb.inode_table_sects,
	sb.data_start_lba);

	struct disk* hd = part->my_disk; 

	// write superblock to the no.1 sector (no.0 is obr)
	ide_write(hd,part->start_lba+1,&sb,1);
	printk("\tsuper_block_lba:0x%x\n",part->start_lba+1);
	
	// find the biggest meta info
	// use it's size as buf_size
	uint32_t buf_size = sb.block_bitmap_sects>=sb.inode_bitmap_sects?sb.block_bitmap_sects:sb.inode_bitmap_sects;
	buf_size = (buf_size>=sb.inode_table_sects?buf_size:sb.inode_table_sects)*SECTOR_SIZE;
	
	uint8_t* buf = (uint8_t*)sys_malloc(buf_size);

	// init block_bitmap and write it to sb.block_bitmap_lba
	buf[0] |= 0x01; // 0th block is used for root dict, reserve it
	uint32_t block_bitmap_last_byte = block_bitmap_bit_len/8;
	uint32_t block_bitmap_last_bit = block_bitmap_bit_len%8;
	uint32_t last_size = SECTOR_SIZE-(block_bitmap_last_byte%SECTOR_SIZE);

	memset(&buf[block_bitmap_last_byte],0xff,last_size);

	uint8_t bit_idx = 0;
	while(bit_idx<=block_bitmap_last_bit){
		buf[block_bitmap_last_byte] &= ~(1<<bit_idx++);
	}
	ide_write(hd,sb.block_bitmap_lba,buf,sb.block_bitmap_sects);


	// init inode_bitmap and write it to sb.inode_bitmap_lba
	// flush the buf
	memset(buf,0,buf_size);
	buf[0] |= 0x1; // reserve for root dict
	ide_write(hd,sb.inode_bitmap_lba,buf,sb.inode_bitmap_sects);

	// init inode list and write it to sb.inode_table_lba
	// flush the buf
	memset(buf,0,buf_size);
	struct inode* i = (struct inode*)buf;
	i->i_size = sb.dir_entry_size*2; // dict '.' and '..'
	i->i_no = 0; // 0th is used for root dict 
	i->i_sectors[0] = sb.data_start_lba;

	ide_write(hd,sb.inode_table_lba,buf,sb.inode_table_sects);

	// write root dict to sb.data_start_lba
	// write dict '.' and '..'
	memset(buf,0,buf_size);
	struct dir_entry* p_de = (struct dir_entry*)buf;

	// init current dict '.'
	memcpy(p_de->filename,".",1);
	p_de->i_no = 0;
	p_de->f_type = FT_DIRECTORY;
	p_de++;

	// init parent dict ".."
	memcpy(p_de->filename,"..",2);
	p_de->i_no = 0;
	p_de->f_type = FT_DIRECTORY;

	// sb.data_start_lba has been allocated to the root dict which contains dict entries
	ide_write(hd,sb.data_start_lba,buf,1);
	
	sys_free(buf);
	printk("\troot_dir_lba:0x%x\n",sb.data_start_lba);
	printk("%s format done\n",part->name);
}

static bool mount_partition(struct dlist_elem* pelem,int arg){
	char* part_name = (char*) arg;
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	if(!strcmp(part->name,part_name)){
		cur_part = part;
		struct disk* hd = cur_part->my_disk;

		// struct super_block* sb_buf = (struct super_block*) sys_malloc(sizeof(struct super_block));
		
		// cur_part->sb = (struct super_block*) sys_malloc(sizeof(struct super_block));
		// if(cur_part->sb == NULL||sb_buf==NULL){
		// 	PANIC("alloc memory failed!");
		// }

		// memset(sb_buf,0,SECTOR_SIZE);
		// ide_read(hd,cur_part->start_lba+1,sb_buf,1);

		// memcpy(cur_part->sb,sb_buf,sizeof(struct super_block));

		cur_part->block_bitmap.bits = (uint8_t*)sys_malloc(cur_part->sb->block_bitmap_sects*SECTOR_SIZE);

		if(cur_part->block_bitmap.bits==NULL){
			PANIC("alloc memory failed!");
		}
		cur_part->block_bitmap.btmp_bytes_len=cur_part->sb->block_bitmap_sects*SECTOR_SIZE;

		ide_read(hd,cur_part->sb->block_bitmap_lba,cur_part->block_bitmap.bits,cur_part->sb->block_bitmap_sects);

		cur_part->inode_bitmap.bits = (uint8_t*)sys_malloc(cur_part->sb->inode_bitmap_sects*SECTOR_SIZE);
		
		if(cur_part->inode_bitmap.bits==NULL){
			PANIC("alloc memory failed!");
		}
		cur_part->inode_bitmap.btmp_bytes_len = cur_part->sb->inode_bitmap_sects*SECTOR_SIZE;

		ide_read(hd,cur_part->sb->inode_bitmap_lba,cur_part->inode_bitmap.bits,cur_part->sb->inode_bitmap_sects);

		dlist_init(&cur_part->open_inodes);
		
		// sys_free(sb_buf);

		printk("mount %s done!\n",part_name);
		
		// printk("%s data_start_lba: %x\n",part_name,part->sb->data_start_lba);
		// printk("%s inode_bitmap_lba: %x\n",part_name,part->sb->inode_bitmap_lba);
		// printk("%s inode_table_lba: %x\n",part_name,part->sb->inode_table_lba);
		// printk("%s block_bitmap_lba: %x\n",part_name,part->sb->block_bitmap_lba);
		return true;
	}
	return false;
}

void filesys_init(){

	uint8_t channel_no = 0,dev_no=0,part_idx = 0;
	bool first_flag = true;

	char default_part[MAX_DISK_NAME_LEN];

	printk("searching filesystem......\n");
	while(channel_no<channel_cnt){
		while (dev_no<2){
			// skip system disk (hd60M.img)
			if(dev_no==0){
				dev_no++;
				continue;
			}
			struct disk* hd = &channels[channel_no].devices[dev_no];
			struct partition* part = hd->prim_parts;
			while(part_idx<12){
				if(part_idx==4){
					part = hd->logic_parts;
				}

				if(part->sec_cnt!=0){
					struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);
					if(sb_buf==NULL){
						PANIC("filesys_init: alloc memory failed!!!");
					}
					memset(sb_buf,0,SECTOR_SIZE);
					ide_read(hd,part->start_lba+1,sb_buf,1);
					if(sb_buf->magic==FS_MAGIC_NUMBER){
						printk("%s has filesystem\n",part->name);
						part->sb = sb_buf;
					} else{
						printk("formatting %s's partition %s ......\n",hd->name,part->name);
						partition_format(part);
						ide_read(hd,part->start_lba+1,sb_buf,1);
						part->sb = sb_buf;
					}
					
					if(first_flag){
						// we regard the first part as default part
						// then, mount the default part;
						strcpy(default_part,part->name);
						first_flag = false;
					}
				}

				part_idx++;
				part++;
			}

			dev_no++;
		}
		channel_no++;
	}
	// sys_free(sb_buf);
	
	// mount default_part, quick mount
	dlist_traversal(&partition_list,mount_partition,(int)default_part);
	open_root_dir(cur_part);
	cur_dir = &root_dir;

	uint32_t fd_idx = 0;
	while(fd_idx<MAX_FILE_OPEN_IN_SYSTEM){
		file_table[fd_idx++].fd_inode = NULL;
	}
}

char* path_parse(char* pathname,char* name_store){
	if(pathname[0]=='/'){
		while(*(++pathname)=='/');
	}

	while(*pathname!='/'&&*pathname!=0){
		*name_store++ = *pathname++;
	}

	if(pathname[0]==0){
		return NULL;
	}
	return pathname;
}

int32_t path_depth_cnt(char* pathname){
	ASSERT(pathname!=NULL);
	char* p = pathname;
	char name[MAX_FILE_NAME_LEN];

	uint32_t depth = 0;

	p = path_parse(p,name);
	while(name[0]){
		depth++;
		memset(name,0,MAX_FILE_NAME_LEN);
		if(p){
			p=path_parse(p,name);
		}
	}
	return depth;
}

static int search_file(const char* pathname,struct path_search_record* searched_record){
	if(!strcmp(pathname,"/")||!strcmp(pathname,"/.")||!strcmp(pathname,"/..")){
			searched_record->parent_dir = &root_dir;
			searched_record->file_type = FT_DIRECTORY;
			searched_record->searched_path[0] = 0;
			return 0;
	}

	uint32_t path_len = strlen(pathname);
	
	ASSERT(pathname[0]=='/'&&path_len>1&&path_len<MAX_PATH_LEN);
	char* sub_path = (char*)pathname;
	struct dir* parent_dir = &root_dir;
	struct dir_entry dir_e;

	char name[MAX_FILE_NAME_LEN]= {0};

	searched_record->parent_dir = parent_dir;
	searched_record->file_type = FT_UNKNOWN;
	uint32_t parent_inode_no = 0;

	sub_path = path_parse(sub_path,name);

	while(name[0]){
		ASSERT(strlen(searched_record->searched_path)<512);

		strcat(searched_record->searched_path,"/");
		strcat(searched_record->searched_path,name);

		if(search_dir_entry(cur_part,parent_dir,name,&dir_e)){
			memset(name,0,MAX_FILE_NAME_LEN);
			if(sub_path){
				sub_path = path_parse(sub_path,name);
			}

			if(FT_DIRECTORY==dir_e.f_type){
				parent_inode_no = parent_dir->inode->i_no;
				dir_close(parent_dir);
				parent_dir = dir_open(cur_part,dir_e.i_no);
				searched_record->parent_dir = parent_dir;
				continue;
			}else if(FT_REGULAR==dir_e.f_type){
				searched_record->file_type = FT_REGULAR;
				return dir_e.i_no;
			}
		}else{
				return -1;
		}
	}
	dir_close(searched_record->parent_dir);

	searched_record->parent_dir = dir_open(cur_part,parent_inode_no);
	searched_record->file_type = FT_DIRECTORY;
	return dir_e.i_no;
}

int32_t sys_open(const char* pathname,uint8_t flags){
	if(pathname[strlen(pathname)-1]=='/'){
		printk("can't open a directory %s\n",pathname);
		return -1;
	}
	ASSERT(flags<=7);
	int32_t fd = -1;

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	
	uint32_t pathname_depth = path_depth_cnt((char*)pathname);
	
	int inode_no = search_file(pathname,&searched_record);
	bool found = inode_no !=-1?true:false;

	if(searched_record.file_type==FT_DIRECTORY){
		printk("can't open a directory with open(), use opendir() to instead\n");
		dir_close(searched_record.parent_dir);
		return -1;
	}

	uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
	// to process the situation /a/b/c (/a/b is a regular file or not exists)
	if(pathname_depth!=path_searched_depth){
		printk("cannot access %s: Not a directory, subpath %s is't exist\n",\
		pathname,searched_record.searched_path);
		return -1;
	}

	if(!found&&!(flags&O_CREATE)){
		printk("in path %s,file %s is't exist\n",\
		searched_record.searched_path,\
		(strrchr(searched_record.searched_path,'/')+1));
		dir_close(searched_record.parent_dir);
		return -1;
	}else if(found && (flags & O_CREATE)){
		printk("%s has already exist!\n",pathname);
		dir_close(searched_record.parent_dir);
		return -1;
	}

	switch (flags&O_CREATE){
		case  O_CREATE:
			printk("creating file\n");
			fd = file_create(searched_record.parent_dir,(strrchr(pathname,'/')+1),flags);
			dir_close(searched_record.parent_dir);
			break;
		default: // file is existed, open the file
			// O_RDONLY,O_WRONLY,O_RDWR
			fd = file_open(inode_no,flags);
			break;
	}
	return fd;
}

uint32_t fd_local2global(uint32_t local_fd){
	struct task_struct* cur = get_running_task_struct();
	int32_t global_fd = cur->fd_table[local_fd];
	ASSERT(global_fd>=0&&global_fd<MAX_FILE_OPEN_IN_SYSTEM);
	return (uint32_t)global_fd;
}
// if success, then return 0
// else return -1
int32_t sys_close(int32_t fd){
	int32_t ret = -1;
	if(fd>2){
		uint32_t global_fd = fd_local2global(fd);
		if(is_pipe(fd)){
			if(--file_table[global_fd].fd_pos==0){
				mfree_page(PF_KERNEL,file_table[global_fd].fd_inode,1);
				file_table[global_fd].fd_inode = NULL;
			}
			ret = 0;
		}else{
			ret = file_close(&file_table[global_fd]);
		}
		get_running_task_struct()->fd_table[fd] = -1;
	}
	return ret;
}

int32_t sys_write(int32_t fd,const void* buf,uint32_t count){
	if(fd<0){
		printk("sys_write: fd error ! fd can not be negtive!\n");
		return -1;
	}

	if(fd==stdout_no){

		if(is_pipe(fd)){
			// console_put_str("stdout redirect to pipe...\n");
			return pipe_write(fd,buf,count);
		}else{
			char tmp_buf[PRINT_BUF_SIZE] = {0};
			memcpy(tmp_buf,buf,count);
			console_put_str(tmp_buf);
			return count;
		}
	}else if(is_pipe(fd)){
		// console_put_str("write to pipe...\n");
		return pipe_write(fd,buf,count);
	}else{
		uint32_t _fd = fd_local2global(fd);
		struct file* wr_file = &file_table[_fd];
		if(wr_file->fd_flag&O_WRONLY||wr_file->fd_flag&O_RDWR){
			uint32_t bytes_written = file_write(wr_file,buf,count);
			return bytes_written;
		}else{
			console_put_str("sys_write: not allowed to write file without flag O_WRONLY or O_RDWR!\n");
			return -1;
		}
	}
}

int32_t sys_read(int32_t fd,void* buf,uint32_t count){
	if(fd<0){
		printk("sys_read: fd error! fd can not be negtive\n");
		return -1;
	}
	ASSERT(buf!=NULL);

	int32_t ret = -1;
	uint32_t global_fd = 0;
	if(fd<0||fd==stdout_no||fd==stderr_no){
		printk("sys_read: fd error!\n");
	}else if(fd==stdin_no){
		if(is_pipe(fd)){
			// console_put_str("stdin redirect to pipe...\n");
			ret = pipe_read(fd,buf,count);
		}else{
			char* buffer = buf;
			uint32_t bytes_read = 0;
			while(bytes_read<count){
				*buffer = ioq_getchar(&kbd_buf);
				bytes_read++;
				buffer++;
			}
			ret = (bytes_read==0?-1:(int32_t)bytes_read);
		}
	}else if(is_pipe(fd)){
		// console_put_str("read from pipe...\n");
		ret = pipe_read(fd,buf,count);
	}else{
		global_fd = fd_local2global(fd);
		ret = file_read(&file_table[global_fd],buf,count); 
	}
	return ret; 
}

int32_t sys_lseek(int32_t fd,int32_t offset,enum whence whence){
	if(fd<0){
		printk("sys_lseek: fd error! fd can not be negtive!\n");
		return -1;
	}
	ASSERT(whence>0&&whence<4);
	uint32_t _fd = fd_local2global(fd);
	struct file* pf = &file_table[_fd];

	int32_t new_pos = 0;
	int32_t file_size = (int32_t)pf->fd_inode->i_size;
	switch (whence){
		case SEEK_SET:
			new_pos = offset;
			break;
		case SEEK_CUR:
			new_pos = (int32_t)pf->fd_pos + offset;
			break;
		case SEEK_END:
			new_pos = file_size+offset;
			break;
		default:
			printk("sys_lseek: error! unknown whence!\n");
			return -1;
	}
	if(new_pos<0||new_pos>(file_size-1)){
		return -1;
	}
	pf->fd_pos = new_pos;
	return pf->fd_pos;
}

int32_t sys_unlink(const char* pathname){
	ASSERT(strlen(pathname)<MAX_PATH_LEN);

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int inode_no = search_file(pathname,&searched_record);
	ASSERT(inode_no!=0);
	if(inode_no==-1){
		printk("file %s not found!\n",pathname);
		dir_close(searched_record.parent_dir);
		return -1;
	}

	if(searched_record.file_type==FT_DIRECTORY){
		printk("can't delete a direcotry with unlink(), use rmdir() instead !\n");
		dir_close(searched_record.parent_dir);
		return -1;
	}

	uint32_t file_idx = 0;
	while(file_idx<MAX_FILE_OPEN_IN_SYSTEM){
		if(file_table[file_idx].fd_inode!=NULL&&(uint32_t)inode_no==file_table[file_idx].fd_inode->i_no){
			break;
		}
		file_idx++;
	}
	if(file_idx<MAX_FILE_OPEN_IN_SYSTEM){
		dir_close(searched_record.parent_dir);
		printk("file %s is in use, not allow to delete!\n",pathname);
		return -1;
	}
	ASSERT(file_idx==MAX_FILE_OPEN_IN_SYSTEM);

	void* io_buf = sys_malloc(SECTOR_SIZE+SECTOR_SIZE);
	if(io_buf==NULL){
		dir_close(searched_record.parent_dir);
		printk("sys_unlink: malloc for io_buf failed!\n");
		return -1;
	}

	struct dir* parent_dir =  searched_record.parent_dir;
	delete_dir_entry(cur_part,parent_dir,inode_no,io_buf);
	inode_release(cur_part,inode_no);
	sys_free(io_buf);
	dir_close(searched_record.parent_dir);
	return 0;
}

int32_t sys_mkdir(const char* pathname){
	uint8_t rollback_step = 0;
	void* io_buf = sys_malloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		printk("sys_mkdir: sys_malloc for io_buf failed!\n");
		return -1;
	}

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int inode_no =-1;
	inode_no = search_file(pathname,&searched_record);

	if(inode_no!=-1){
		printk("sys_mkdir: file or directory %s is exist!\n",pathname);
		rollback_step = 1;
		goto rollback;
	}else{
		uint32_t pathname_depth = path_depth_cnt((char*) pathname);
		uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path); 
		if(pathname_depth!=path_searched_depth){
			printk("sys_mkdir: cannot access %s: Not a directory, subpath %s is't exist\n",pathname,searched_record.searched_path);
			rollback_step = 1;
			goto rollback;
		}
	}

	struct dir* parent_dir = searched_record.parent_dir;
	char* dir_name = strrchr(searched_record.searched_path,'/')+1;
	inode_no = inode_bitmap_alloc(cur_part);
	if(inode_no==-1){
		printk("sys_mkdir: allocate inode failed\n");
		rollback_step = 1;
		goto rollback;
	}

	struct inode new_dir_inode;
	inode_init(inode_no,&new_dir_inode);

	uint32_t block_bitmap_idx = 0;
	int32_t block_lba = -1;

	block_lba = block_bitmap_alloc(cur_part);
	if(block_lba==-1){
		printk("sys_mkdir: block_bitmap_alloc for create directory failed!\n");
		rollback_step = 2;
		goto rollback;
	}

	new_dir_inode.i_sectors[0] = block_lba;

	block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
	ASSERT(block_bitmap_idx!=0);
	

	memset(io_buf,0,SECTOR_SIZE*2);
	struct dir_entry* p_de = (struct dir_entry*) io_buf;


	memcpy(p_de->filename,".",1);
	p_de->i_no = inode_no;
	p_de->f_type = FT_DIRECTORY;

	p_de++;
	memcpy(p_de->filename,"..",2);
	p_de->i_no = parent_dir->inode->i_no;
	p_de->f_type = FT_DIRECTORY;
	ide_write(cur_part->my_disk,new_dir_inode.i_sectors[0],io_buf,1);

	new_dir_inode.i_size = 2*cur_part->sb->dir_entry_size;

	struct dir_entry new_dir_entry;
	memset(&new_dir_entry,0,sizeof(struct dir_entry));
	create_dir_entry(dir_name,inode_no,FT_DIRECTORY,&new_dir_entry);
	memset(io_buf,0,SECTOR_SIZE*2);
	if(!sync_dir_entry(parent_dir,&new_dir_entry,io_buf)){
		printk("sys_mkdir: sync_dir_entry to disk failed!\n");
		rollback_step = 2;
		goto rollback;
	}

    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(cur_part, parent_dir->inode, io_buf);

	memset(io_buf,0,SECTOR_SIZE*2);
	inode_sync(cur_part,&new_dir_inode,io_buf);

	

	bitmap_sync(cur_part,block_bitmap_idx,BLOCK_BITMAP);
	bitmap_sync(cur_part,inode_no,INODE_BITMAP);

	sys_free(io_buf);

	dir_close(searched_record.parent_dir);
	return 0;

rollback:
	switch (rollback_step){
		case  2:
			bitmap_set(&cur_part->inode_bitmap,inode_no,0);
		case 1:
			dir_close(searched_record.parent_dir);
			break;
	}
	sys_free(io_buf);
	return -1;
}

struct dir* sys_opendir(const char* name){
	ASSERT(strlen(name)<MAX_PATH_LEN);
	if(name[0]=='/'&&(name[1]==0||name[1]=='.')){
		return &root_dir;
	}

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int inode_no = search_file(name,&searched_record);
	struct dir* ret = NULL;
	if(inode_no==-1){
		printk("In %s, sub path %s not exists\n",name,searched_record.searched_path);
	}else{
		if(searched_record.file_type==FT_REGULAR){
			printk("%s is regular file!\n",name);
		}else if(searched_record.file_type==FT_DIRECTORY){
			ret = dir_open(cur_part,inode_no);
		}
	}

	dir_close(searched_record.parent_dir);
	return ret;
}

int32_t sys_closedir(struct dir* dir){
	int32_t ret = -1;
	if(dir!=NULL){
		dir_close(dir);
		ret = 0;
	}
	return ret;
}

struct dir_entry* sys_readdir(struct dir* dir){
	ASSERT(dir!=NULL);
	return dir_read(dir);
}

void sys_rewinddir(struct dir* dir){
	dir->dir_pos = 0;
}

int32_t sys_rmdir(const char* pathname){
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int inode_no = search_file(pathname,&searched_record);
	ASSERT(inode_no!=0);
	int retval = -1;
	if(inode_no==-1){
		printk("In %s, sub path %s not exist\n",pathname,searched_record.searched_path);
	}else{
		if(searched_record.file_type==FT_REGULAR){
			printk("%s is regular file!\n",pathname);
		}else{
			struct dir* dir = dir_open(cur_part,inode_no);
			if(!dir_is_empty(dir)){
				printk("dir %s is not empty, it is not allowed to delete a nonempty directory!\n",pathname);
			}else{
				if(!dir_remove(searched_record.parent_dir,dir)){
					retval = 0;
				}
			}
			dir_close(dir);
		}
	}
	dir_close(searched_record.parent_dir);
	return retval;
}

static uint32_t get_parent_dir_inode_nr(uint32_t child_inode_nr,void* io_buf){
	struct inode* child_dir_inode = inode_open(cur_part,child_inode_nr);
	uint32_t block_lba = child_dir_inode->i_sectors[0];
	ASSERT(block_lba>=cur_part->sb->data_start_lba);
	inode_close(child_dir_inode);
	ide_read(cur_part->my_disk,block_lba,io_buf,1);
	struct dir_entry* dir_e = (struct dir_entry*)io_buf;

	ASSERT(dir_e[1].i_no<MAX_FILES_PER_PART&&dir_e[1].f_type==FT_DIRECTORY);
	return dir_e[1].i_no;
}

static int get_child_dir_name(uint32_t p_inode_nr,uint32_t c_inode_nr,char* path,void* io_buf){
	struct inode* parent_dir_inode = inode_open(cur_part,p_inode_nr);
	uint8_t block_idx = 0;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0},block_cnt = DIRECT_INDEX_BLOCK;
	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = parent_dir_inode->i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(parent_dir_inode->i_sectors[tfflib]){
		ide_read(cur_part->my_disk,parent_dir_inode->i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;
	}
	inode_close(parent_dir_inode);

	struct dir_entry* dir_e = (struct dir_entry*) io_buf;
	uint32_t dir_entry_size = cur_part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec =(SECTOR_SIZE/dir_entry_size);

	block_idx = 0;

	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]){
			ide_read(cur_part->my_disk,all_blocks_addr[block_idx],io_buf,1);
			uint8_t dir_e_idx = 0;
			while(dir_e_idx<dir_entrys_per_sec){
				if((dir_e+dir_e_idx)->i_no==c_inode_nr){
					strcat(path,"/");
					strcat(path,(dir_e+dir_e_idx)->filename);
					return 0;
				}
				dir_e_idx++;
			}
		}
		block_idx++;
	}
	return -1;
}

char* sys_getcwd(char* buf,uint32_t size){
	ASSERT(buf!=NULL);
	void* io_buf = sys_malloc(SECTOR_SIZE);
	if(io_buf==NULL){
		return NULL;
	}
	
	struct task_struct* cur_thread = get_running_task_struct();
	int32_t parent_inode_nr = 0;
	int32_t child_inode_nr = cur_thread->cwd_inode_nr;
	ASSERT(child_inode_nr>=0&&child_inode_nr<MAX_FILES_PER_PART);

	
	
	if(child_inode_nr==0){
		buf[0] = '/';
		buf[1] = 0;
		sys_free(io_buf);
		return buf;
	}

	memset(buf,0,size);
	char full_path_reverse[MAX_PATH_LEN] = {0};
	
	while ((child_inode_nr)){
		parent_inode_nr = get_parent_dir_inode_nr(child_inode_nr,io_buf);
		if(get_child_dir_name(parent_inode_nr,child_inode_nr,full_path_reverse,io_buf)==-1){
			sys_free(io_buf);
			return NULL;
		}
		child_inode_nr = parent_inode_nr;
	}

	ASSERT(strlen(full_path_reverse)<=size);

	char* last_slash;
	while((last_slash=strrchr(full_path_reverse,'/'))){
		uint16_t len = strlen(buf);
		strcpy(buf+len,last_slash);

		*last_slash = 0;
	}
	sys_free(io_buf);
	
	return buf;
}

int32_t sys_chdir(const char* path){
	int32_t ret = -1;
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));

	int inode_no = search_file(path,&searched_record);
	if(inode_no!=-1){
		if(searched_record.file_type==FT_DIRECTORY){
			get_running_task_struct()->cwd_inode_nr = inode_no;
			ret = 0;
		}else{
			printk("sys_chdir: %s is regular file or other!\n",path);
		}
	}else{
		printk("sys_chdir: path %s maybe not exist\n",searched_record.searched_path);
	}
	dir_close(searched_record.parent_dir);
	return ret;
}

int32_t sys_stat(const char* path,struct stat* buf){
	if(!strcmp(path,"/")||!strcmp(path,"/.")||!strcmp(path,"/..")){
		buf->st_filetype = FT_DIRECTORY;
		buf->st_ino = 0;
		buf->st_size = root_dir.inode->i_size;
		return 0;
	}

	int32_t ret = -1;
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int32_t inode_no = search_file(path,&searched_record);

	if(inode_no!=-1){
		struct inode* obj_inode = inode_open(cur_part,inode_no); 
		buf->st_size = obj_inode->i_size;
		inode_close(obj_inode);
		buf->st_filetype = searched_record.file_type;
		buf->st_ino = inode_no;
		ret = 0;

	}else{	
		printk("sys_stat: %s not found!\n",path);
	}

	dir_close(searched_record.parent_dir);
	return ret;
}
// partition formatlize 512B-blocks used avaliable
void sys_disk_info(){
	uint8_t channel_idx;
	printk("disk number: %d\n",disk_num);
	uint8_t k = 0;
	char** granularits = sys_malloc(sizeof(char*)*disk_num);
	uint8_t* div_cnts = sys_malloc(sizeof(uint8_t)*disk_num);
	int i=0;
	for(i=0;i<disk_num;i++){
		while(disk_size[i]>1024){
			disk_size[i]/=1024;
			div_cnts[i]++;
		}
		switch (div_cnts[i]){
			case 0:
				granularits[i] = "B";
				break;
			case 1:
				granularits[i] = "KB";
				break;
			case 2:
				granularits[i] = "MB";
				break;
			case 3:
				granularits[i] = "GB";
				break;
			case 4:
				granularits[i] = "TB";
				break;
			default:
				granularits[i] = "OVERFLOW!";
				break;
		}
	}

	for(channel_idx=0;channel_idx<CHANNEL_NUM;channel_idx++){
		uint8_t ide_idx = 0;
		for(ide_idx=0;ide_idx<DEVICE_NUM_PER_CHANNEL;ide_idx++){
			if(!strcmp("",channels[channel_idx].devices[ide_idx].name)) continue;
			printk("%s\t%d%s\n",channels[channel_idx].devices[ide_idx].name,disk_size[channel_idx*DISK_NUM_IN_CHANNEL+ide_idx],granularits[channel_idx*DISK_NUM_IN_CHANNEL+ide_idx]);
		}
	}


	printk("partition\tformatlize\t512B-blocks\tused\tavaliable\n");
	for(channel_idx=0;channel_idx<CHANNEL_NUM;channel_idx++){
		uint8_t device_idx = 0;
		bool has_partition = false;
		for(device_idx=0;device_idx<DEVICE_NUM_PER_CHANNEL;device_idx++){
			if(!strcmp("",channels[channel_idx].devices[device_idx].name)) continue;
			uint8_t part_idx = 0;
			for(part_idx=0;part_idx<PRIM_PARTS_NUM;part_idx++){
				if(!strcmp("",channels[channel_idx].devices[device_idx].prim_parts[part_idx].name)) continue;
				has_partition = true;
			}
			if(has_partition){
				for(part_idx=0;part_idx<PRIM_PARTS_NUM;part_idx++){
					if(!strcmp("",channels[channel_idx].devices[device_idx].prim_parts[part_idx].name)) continue;
					uint32_t bitmap_sects = channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->block_bitmap_sects;
					uint8_t* buf_bitmap_bits = sys_malloc(bitmap_sects*SECTOR_SIZE);
					memset(buf_bitmap_bits,0,bitmap_sects*SECTOR_SIZE);

					ide_read(&channels[channel_idx].devices[device_idx]
						,channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->block_bitmap_lba
						,buf_bitmap_bits
						,bitmap_sects
					);
					
					struct bitmap bitmap_buf;
					bitmap_buf.bits = buf_bitmap_bits;
					bitmap_buf.btmp_bytes_len = (channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->sec_cnt)/8;
					uint32_t free_sects = bitmap_count(&bitmap_buf);
					uint32_t used_sects = (bitmap_buf.btmp_bytes_len*8)-free_sects;
					sys_free(buf_bitmap_bits);
					// P means primary part
					char cur_flag_str[2] = {0};
					if(!strcmp(cur_part->name,channels[channel_idx].devices[device_idx].prim_parts[part_idx].name)){
						cur_flag_str[0] = '*';
					}
					printk("%s(P)%s\t%x\t%d\t%d\t%d\n"
					,channels[channel_idx].devices[device_idx].prim_parts[part_idx].name
					,cur_flag_str
					,channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->magic
					,channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->sec_cnt
					,used_sects
					,free_sects);
				}
				for(part_idx=0;part_idx<LOGIC_PARTS_NUM;part_idx++){
					if(!strcmp("",channels[channel_idx].devices[device_idx].logic_parts[part_idx].name)) continue;
					uint32_t bitmap_sects = channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->block_bitmap_sects;
					uint8_t* buf_bitmap_bits = sys_malloc(bitmap_sects*SECTOR_SIZE);
					memset(buf_bitmap_bits,0,bitmap_sects*SECTOR_SIZE);

					ide_read(&channels[channel_idx].devices[device_idx]
						,channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->block_bitmap_lba
						,buf_bitmap_bits
						,bitmap_sects
					);
					
					struct bitmap bitmap_buf;
					bitmap_buf.bits = buf_bitmap_bits;
					bitmap_buf.btmp_bytes_len = (channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->sec_cnt)/8;
					uint32_t free_sects = bitmap_count(&bitmap_buf);
					uint32_t used_sects = (bitmap_buf.btmp_bytes_len*8)-free_sects;
					sys_free(buf_bitmap_bits);
					char cur_flag_str[2] = {0};
					if(!strcmp(cur_part->name,channels[channel_idx].devices[device_idx].logic_parts[part_idx].name)){
						cur_flag_str[0] = '*';
					}

					// uint32_t sum = 1+1+channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->inode_bitmap_sects+channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->block_bitmap_sects+channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->inode_table_sects;
					// printk("used: %d\n",sum);
					// L means logic part
					printk("%s(L)%s\t%x\t%d\t%d\t%d\n"
					,channels[channel_idx].devices[device_idx].logic_parts[part_idx].name
					,cur_flag_str
					,channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->magic
					,channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->sec_cnt
					,used_sects
					,free_sects);
				}
			}else{
				// R means raw disk
				printk("%s(R)\t-\t-\t-\t%d%s\n",channels[channel_idx].devices[device_idx].name,disk_size[channel_idx*DISK_NUM_IN_CHANNEL+device_idx],granularits[channel_idx*DISK_NUM_IN_CHANNEL+device_idx]);
			}
		}
	}
}

bool check_disk_name(struct dlist_elem* pelem,int arg){
	char* part_name = (char*) arg;
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	if(!strcmp(part_name,part->name)){
		return true;
	}
	return false;
}

void sys_mount(const char* part_name){
	
	struct dlist_elem* res = dlist_traversal(&partition_list,check_disk_name,(int)part_name);
	if(res==NULL){
		printk("sys_mount: partition %s not found!\n",part_name);
		return;
	}

	if(cur_part!=NULL&&cur_part->block_bitmap.bits!=NULL){
		// printk("remove block bitmap\n");
		sys_free(cur_part->block_bitmap.bits);
		cur_part->block_bitmap.btmp_bytes_len = 0;
	}

	if(cur_part!=NULL&&cur_part->inode_bitmap.bits!=NULL){
		// printk("remove inode bitmap\n");
		sys_free(cur_part->inode_bitmap.bits);
		cur_part->inode_bitmap.btmp_bytes_len = 0;
	}

	if(cur_part!=NULL){
		close_root_dir(cur_part);
		printk("close root directory\n");
	} 

	dlist_traversal(&partition_list,mount_partition,(int)part_name);
	open_root_dir(cur_part);
	printk("partition %s mounted\n", cur_part->name);
}