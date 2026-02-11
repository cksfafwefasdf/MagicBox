#include "fs.h"
#include "dir.h"
#include "debug.h"
#include "string.h"
#include "stdio-kernel.h"
#include "stdbool.h"
#include "dlist.h"
#include "console.h"
#include "thread.h"
#include "ioqueue.h"
#include "keyboard.h"
#include "pipe.h"
#include "bitmap.h"
#include "memory.h"
#include "tty.h"
#include "device.h"
#include "fs_types.h"
#include "stdio.h"
#include "ide.h"
#include "file.h"
#include "inode.h"
#include "ide_buffer.h"

struct partition* cur_part;
struct dir* cur_dir;

// logic formatlize 
static void partition_format(struct partition* part){
	uint32_t boot_sector_sects = 1;
	uint32_t super_block_sects = 1;
	uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART,BITS_PER_SECTOR);
	uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct d_inode)*MAX_FILES_PER_PART)),SECTOR_SIZE);
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
	bwrite_multi(hd,part->start_lba+1,&sb,1);
	printk("\tsuper_block_lba:0x%x\n",part->start_lba+1);
	
	// find the biggest meta info
	// use it's size as buf_size
	uint32_t buf_size = sb.block_bitmap_sects>=sb.inode_bitmap_sects?sb.block_bitmap_sects:sb.inode_bitmap_sects;
	buf_size = (buf_size>=sb.inode_table_sects?buf_size:sb.inode_table_sects)*SECTOR_SIZE;
	
	uint8_t* buf = (uint8_t*)kmalloc(buf_size);

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
	bwrite_multi(hd,sb.block_bitmap_lba,buf,sb.block_bitmap_sects);


	// init inode_bitmap and write it to sb.inode_bitmap_lba
	// flush the buf
	memset(buf,0,buf_size);
	buf[0] |= 0x1; // reserve for root dict
	bwrite_multi(hd,sb.inode_bitmap_lba,buf,sb.inode_bitmap_sects);

	// init inode list and write it to sb.inode_table_lba
	// flush the buf
	memset(buf,0,buf_size);
	struct d_inode* i = (struct d_inode*)buf;
	i->i_size = sb.dir_entry_size*2; // dict '.' and '..'
	// i->i_no = 0; // 0th is used for root dict 
	i->i_sectors[0] = sb.data_start_lba;
	i->i_type = FT_DIRECTORY;

	bwrite_multi(hd,sb.inode_table_lba,buf,sb.inode_table_sects);

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
	bwrite_multi(hd,sb.data_start_lba,buf,1);
	
	kfree(buf);
	printk("\troot_dir_lba:0x%x\n",sb.data_start_lba);
	printk("%s format done\n",part->name);
}

static bool mount_partition(struct dlist_elem* pelem,void* arg){
	char* part_name = (char*) arg;
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	if(!strcmp(part->name,part_name)){
		cur_part = part;
		struct disk* hd = cur_part->my_disk;
		
		cur_part->block_bitmap.bits = (uint8_t*)kmalloc(cur_part->sb->block_bitmap_sects*SECTOR_SIZE);
		cur_part->inode_bitmap.bits = (uint8_t*)kmalloc(cur_part->sb->inode_bitmap_sects*SECTOR_SIZE);
		

		
		if(cur_part->block_bitmap.bits==NULL){
			PANIC("alloc memory failed!");
		}
		cur_part->block_bitmap.btmp_bytes_len=cur_part->sb->block_bitmap_sects*SECTOR_SIZE;
		
		bread_multi(hd,cur_part->sb->block_bitmap_lba,cur_part->block_bitmap.bits,cur_part->sb->block_bitmap_sects);
		
	
		if(cur_part->inode_bitmap.bits==NULL){
			PANIC("alloc memory failed!");
		}
		cur_part->inode_bitmap.btmp_bytes_len = cur_part->sb->inode_bitmap_sects*SECTOR_SIZE;
		bread_multi(hd,cur_part->sb->inode_bitmap_lba,cur_part->inode_bitmap.bits,cur_part->sb->inode_bitmap_sects);
		dlist_init(&cur_part->open_inodes);
		
		// kfree(sb_buf);

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
					struct super_block* sb_buf = (struct super_block*)kmalloc(SECTOR_SIZE);
					if(sb_buf==NULL){
						PANIC("filesys_init: alloc memory failed!!!");
					}
					memset(sb_buf,0,SECTOR_SIZE);
					bread_multi(hd,part->start_lba+1,sb_buf,1);
					if(sb_buf->magic==FS_MAGIC_NUMBER){
						printk("%s has filesystem\n",part->name);
						part->sb = sb_buf;
					} else{
						printk("formatting %s's partition %s ......\n",hd->name,part->name);
						partition_format(part);
						bread_multi(hd,part->start_lba+1,sb_buf,1);
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
	// kfree(sb_buf);
	
	// mount default_part, quick mount
	dlist_traversal(&partition_list,mount_partition,(void*)default_part);
	open_root_dir(cur_part);
	cur_dir = &root_dir;

	uint32_t fd_idx = 0;
	while(fd_idx<MAX_FILE_OPEN_IN_SYSTEM){
		file_table[fd_idx++].fd_inode = NULL;
	}
}

char* _path_parse(char* pathname,char* name_store){
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

	p = _path_parse(p,name);
	while(name[0]){
		depth++;
		memset(name,0,MAX_FILE_NAME_LEN);
		if(p){
			p=_path_parse(p,name);
		}
	}
	return depth;
}

// search_file 只关闭中间搜索过程中的“中间父目录”，最终的父目录无论如何他都不关闭
// 由调用者关闭
// static int search_file(const char* pathname,struct path_search_record* searched_record){
// 	if(!strcmp(pathname,"/")||!strcmp(pathname,"/.")||!strcmp(pathname,"/..")){
// 			searched_record->parent_dir = dir_open(cur_part, 0);
// 			searched_record->file_type = FT_DIRECTORY;
// 			searched_record->searched_path[0] = 0;
// 			searched_record->i_dev = root_dir.inode->i_dev;
// 			return 0;
// 	}
	

// 	uint32_t path_len = strlen(pathname);
// 	ASSERT(pathname[0]=='/'&&path_len>1&&path_len<MAX_PATH_LEN);
// 	char* sub_path = (char*)pathname;
// 	// struct dir* parent_dir = &root_dir;
// 	// 重复打开一遍 root_dir ，防止在 sys_open 中错误的关闭全局变量的 root_dir

// 	struct dir* parent_dir = dir_open(cur_part, 0); 
// 	struct dir_entry dir_e;

	

// 	char name[MAX_FILE_NAME_LEN]= {0};

// 	searched_record->parent_dir = parent_dir;
// 	searched_record->file_type = FT_UNKNOWN;
// 	searched_record->i_dev = parent_dir->inode->i_dev;

// 	uint32_t parent_inode_no = 0;

// 	struct partition* part;

// 	sub_path = _path_parse(sub_path,name);

// 	while(name[0]){
// 		ASSERT(strlen(searched_record->searched_path)<512);

// 		part = get_part_by_rdev(searched_record->i_dev);

// 		strcat(searched_record->searched_path,"/");
// 		strcat(searched_record->searched_path,name);

// 		if(search_dir_entry(part,parent_dir,name,&dir_e)){
// 			memset(name,0,MAX_FILE_NAME_LEN);
// 			if(sub_path){
// 				sub_path = _path_parse(sub_path,name);
// 			}

// 			if(FT_DIRECTORY==dir_e.f_type){
// 				parent_inode_no = parent_dir->inode->i_no;

// 				if (parent_dir != &root_dir) { 
// 					dir_close(parent_dir); 
// 				}

// 				parent_dir = dir_open(part,dir_e.i_no);
// 				searched_record->parent_dir = parent_dir;
// 				searched_record->i_dev = parent_dir->inode->i_dev;
// 				continue;
// 			}else if(FT_REGULAR==dir_e.f_type){
// 				searched_record->file_type = FT_REGULAR;
// 				return dir_e.i_no;
// 			}
// 		}else{
// 			return -1;
// 		}
// 	}

// 	part = get_part_by_rdev(searched_record->parent_dir->inode->i_dev);
// 	dir_close(searched_record->parent_dir);
// 	searched_record->parent_dir = dir_open(part,parent_inode_no);
// 	searched_record->file_type = FT_DIRECTORY;
// 	return dir_e.i_no;
// }

// search_file 只关闭中间搜索过程中的“中间父目录”，最终的父目录无论如何他都不关闭
// 由调用者关闭
static int search_file(const char* pathname, struct path_search_record* searched_record) {
    // 处理根目录特例
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/..")) {
        searched_record->parent_dir = dir_open(cur_part, 0);
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0;
        searched_record->i_dev = root_dir.inode->i_dev;
        return 0;
    }

    uint32_t path_len = strlen(pathname);
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);
    
    char* sub_path = (char*)pathname;
    struct dir* parent_dir = dir_open(cur_part, 0); 
    struct dir_entry dir_e;
    char name[MAX_FILE_NAME_LEN] = {0};

    searched_record->parent_dir = parent_dir;
    searched_record->file_type = FT_UNKNOWN;
    searched_record->i_dev = parent_dir->inode->i_dev;

    uint32_t parent_inode_no = 0; // 用于记录当前目录的父目录inode
    struct partition* part;

    sub_path = _path_parse(sub_path, name);

    while (name[0]) {
        ASSERT(strlen(searched_record->searched_path) < 512);

        part = get_part_by_rdev(searched_record->i_dev);

        // 记录已经扫描过的路径
        strcat(searched_record->searched_path, "/");
        strcat(searched_record->searched_path, name);

        // 在当前目录查找该名字
        if (search_dir_entry(part, parent_dir, name, &dir_e)) {
            memset(name, 0, MAX_FILE_NAME_LEN);
            if (sub_path) {
                sub_path = _path_parse(sub_path, name);
            }

            if (FT_DIRECTORY == dir_e.f_type) {
                // 如果是目录，则进入该目录继续查找
                parent_inode_no = parent_dir->inode->i_no;
                if (parent_dir != &root_dir) { 
                    dir_close(parent_dir); 
                }
                parent_dir = dir_open(part, dir_e.i_no);
                searched_record->parent_dir = parent_dir;
                searched_record->i_dev = parent_dir->inode->i_dev;
                continue;
            } else {
                // 只要不是目录（包括普通文件、字符设备、块设备），就说明已经找到了目标路径的最末端
				// 直接返回 
                searched_record->file_type = dir_e.f_type;
                return dir_e.i_no;
            }
        } else {
            // 找不到该名字，直接返回-1，由调用者决定是报错还是创建文件
            return -1;
        }
    }

    // 如果执行到这里，说明循环是因为 name[0] 为空退出的，
    // 这意味着路径以 "/" 结尾，且最后一个名字也是目录（例如 "/dev/"）
    
    // 为了符合 sys_open 的逻辑，我们需要让 parent_dir 指向该目录的父目录
    part = get_part_by_rdev(searched_record->parent_dir->inode->i_dev);
    dir_close(searched_record->parent_dir);
    searched_record->parent_dir = dir_open(part, parent_inode_no);
    
    searched_record->file_type = FT_DIRECTORY;
    return dir_e.i_no;
}

int32_t sys_open(const char* pathname,uint8_t flags){
	// printk("sys_open:::pathname: %s\n",pathname);
	if(pathname[strlen(pathname)-1]=='/'){
		printk("can't open a directory %s\n",pathname);
		return -1;
	}
	// ASSERT(flags<=7);
	int32_t fd = -1;

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	
	uint32_t pathname_depth = path_depth_cnt((char*)pathname);
	
	int inode_no = search_file(pathname,&searched_record);
	bool found = inode_no !=-1?true:false;

	// 只有找到了文件，才去校验它是不是目录
	if (found && searched_record.file_type == FT_DIRECTORY) {
		printk("can't open a directory with open()...\n");
		dir_close(searched_record.parent_dir);
		return -1;
	}

	uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
	// to process the situation /a/b/c (/a/b is a regular file or not exists)
	if(pathname_depth!=path_searched_depth){
		printk("cannot access %s: Not a directory, subpath %s is't exist\n",\
		pathname,searched_record.searched_path);
		dir_close(searched_record.parent_dir);
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
			// O_RDONLY,O_WRONLY,O_RDWRget_part_by_rdev
			struct partition* part = get_part_by_rdev(searched_record.i_dev);
			fd = file_open(part,inode_no,flags);
			dir_close(searched_record.parent_dir); // 关掉父文件夹，解析任务已经完成了
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
int32_t sys_close(int32_t fd) {
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }

    struct task_struct* cur = get_running_task_struct();
    int32_t global_fd = cur->fd_table[fd];

    if (global_fd == -1) {
        return -1; 
    }

    struct file* file = &file_table[global_fd];

    // 如果是管道，先执行管道特有的逻辑
    // 无论 f_count 是多少，只要本进程关闭了这一个 FD，
    // 就应该对应地减少管道的 reader/writer_count 并唤醒对端。
	// 这些操作都会在 pipe_release 做
    if (file->fd_inode->di.i_type == FT_PIPE) {
        pipe_release(file);
    }

    // 调用通用的文件关闭函数，处理 f_count 和 inode_close
    int32_t ret = file_close(file);

    // 释放局部资源, 本进程的局部 fd 槽位回收
    cur->fd_table[fd] = -1;

    return ret;
}

// 将 buf 中 count 个字节写入文件描述符 fd
int32_t sys_write(int32_t fd,void* buf, uint32_t count) {
    if (fd < 0) {
        printk("sys_write: fd error!\n");
        return -1;
    }

    // 获取全局文件结构体
    int32_t _fd = fd_local2global(fd);
    struct file* wr_file = &file_table[_fd];

    // 权限检查
    if (!(wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR)) {
        return -1;
    }

    struct m_inode* inode = wr_file->fd_inode;
    ASSERT(inode != NULL);

    // 根据 inode 类型进行分发
    enum file_types type = inode->di.i_type;

    switch (type) {
        case FT_PIPE:
            // 管道逻辑
            return pipe_write(wr_file, buf, count);

        case FT_CHAR_SPECIAL:
            // 字符设备逻辑
            uint32_t major = MAJOR(inode->di.i_rdev);
            if (major == TTY_MAJOR) return tty_write(buf, count);
            if (major == CONSOLE_MAJOR) return console_dev_write(wr_file, buf, count);
			printk("sys_write: char device major %d not supported!\n", major);
            return -1;

        case FT_BLOCK_SPECIAL:
            // 块设备逻辑
            return ide_dev_write(wr_file, buf, count);

        case FT_REGULAR:
        case FT_DIRECTORY:
            // 普通文件或目录逻辑
            return file_write(wr_file, buf, count);

        default:
            printk("sys_write: unknown file type %d!\n", type);
            return -1;
    }
}

// 从文件描述符 fd 中读取 count 个字节到 buf
int32_t sys_read(int32_t fd, void* buf, uint32_t count) {
    if (fd < 0) {
        printk("sys_read: fd error! fd cannot be negative\n");
        return -1;
    }
    ASSERT(buf != NULL);

    // 获取全局文件结构体
    int32_t global_fd_idx = fd_local2global(fd);
    struct file* rd_file = &file_table[global_fd_idx];

    // 权限检查
    if (!(rd_file->fd_flag & O_RDONLY || rd_file->fd_flag & O_RDWR)) {
        return -1;
    }

    struct m_inode* inode = rd_file->fd_inode;
    ASSERT(inode != NULL);

    // 根据 Inode 类型进行统一分发
    enum file_types type = inode->di.i_type;

    switch (type) {
        case FT_PIPE:
            // 管道逻辑
            return pipe_read(rd_file, buf, count);

        case FT_CHAR_SPECIAL: {
            // 字符设备分发
            uint32_t major = MAJOR(inode->di.i_rdev);
            if (major == TTY_MAJOR) {
                return tty_read(buf, count);
            }
            // 以后在此可以扩展其他字符设备
            printk("sys_read: char device major %d read not supported!\n", major);
            return -1;
        }

        case FT_BLOCK_SPECIAL:
            // 块设备分发 (如直接读分区数据)
            return ide_dev_read(rd_file, buf, count);

        case FT_REGULAR:
        case FT_DIRECTORY:
            // 普通磁盘文件或目录逻辑
            return file_read(rd_file, buf, count);

        default:
            printk("sys_read: unknown file type %d!\n", type);
            return -1;
    }
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
	int32_t file_size = (int32_t)pf->fd_inode->di.i_size;
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

	void* io_buf = kmalloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		dir_close(searched_record.parent_dir);
		printk("sys_unlink: malloc for io_buf failed!\n");
		return -1;
	}

	struct dir* parent_dir =  searched_record.parent_dir;
	struct partition* part = get_part_by_rdev(parent_dir->inode->i_dev);
	delete_dir_entry(part,parent_dir,inode_no,io_buf);
	inode_release(part,inode_no);
	kfree(io_buf);
	dir_close(searched_record.parent_dir);
	return 0;
}

int32_t sys_mkdir(const char* pathname){
	uint8_t rollback_step = 0;
	void* io_buf = kmalloc(SECTOR_SIZE*2);
	if(io_buf==NULL){
		printk("sys_mkdir: kmalloc for io_buf failed!\n");
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

	struct partition* part =  get_part_by_rdev(searched_record.i_dev);

	inode_no = inode_bitmap_alloc(part);
	if(inode_no==-1){
		printk("sys_mkdir: allocate inode failed\n");
		rollback_step = 1;
		goto rollback;
	}

	struct m_inode new_dir_inode;
	inode_init(part,inode_no,&new_dir_inode,FT_DIRECTORY);

	uint32_t block_bitmap_idx = 0;
	int32_t block_lba = -1;

	block_lba = block_bitmap_alloc(part);
	if(block_lba==-1){
		printk("sys_mkdir: block_bitmap_alloc for create directory failed!\n");
		rollback_step = 2;
		goto rollback;
	}

	new_dir_inode.di.i_sectors[0] = block_lba;

	block_bitmap_idx = block_lba - part->sb->data_start_lba;
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
	bwrite_multi(part->my_disk,new_dir_inode.di.i_sectors[0],io_buf,1);

	new_dir_inode.di.i_size = 2*part->sb->dir_entry_size;

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
	// printk("sys_mkdir:::part:%x dir_inode:%x\n",part->i_rdev,parent_dir->inode->i_dev);
    inode_sync(part, parent_dir->inode, io_buf);

	memset(io_buf,0,SECTOR_SIZE*2);
	// printk("sys_mkdir:::part:%x dir_inode:%x\n",part->i_rdev,new_dir_inode.i_dev);
	inode_sync(part,&new_dir_inode,io_buf);

	

	bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
	bitmap_sync(part,inode_no,INODE_BITMAP);

	kfree(io_buf);

	dir_close(searched_record.parent_dir);
	return 0;

rollback:
	switch (rollback_step){
		case  2:
			bitmap_set(&part->inode_bitmap,inode_no,0);
		case 1:
			dir_close(searched_record.parent_dir);
			break;
	}
	kfree(io_buf);
	return -1;
}

int32_t sys_opendir(const char* name) {
    ASSERT(strlen(name) < MAX_PATH_LEN);
    
    struct path_search_record searched_record;
    // 初始化结构体
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    int inode_no = -1;
	struct partition *part;

    // 处理根目录
    if (strcmp(name, "/") == 0 || strcmp(name, "/.") == 0 || strcmp(name, "/..") == 0) {
        inode_no = cur_part->sb->root_inode_no;
        searched_record.file_type = FT_DIRECTORY;
        // 根目录没有父目录，设为 NULL 以免后面 dir_close 出错
        searched_record.parent_dir = NULL; 
		part = cur_part;
    } else {
        // search_file 内部会填充 searched_record
        inode_no = search_file(name, &searched_record);
		part = get_part_by_rdev(searched_record.i_dev);
    }

    // 校验是否找到以及类型是否正确
    if (inode_no == -1) {
        printk("opendir: %s not found\n", name);
        return -1;
    }

    if (searched_record.file_type != FT_DIRECTORY) {
        printk("opendir: %s is not a directory, type is %d\n", name, searched_record.file_type);
        // 如果 search_file 打开了父目录，需要关闭
        if (searched_record.parent_dir) {
            dir_close(searched_record.parent_dir);
        }
        return -1;
    }

    // 分配全局文件表槽位
    int32_t fd_idx = get_free_slot_in_global();
    if (fd_idx == -1) {
        if (searched_record.parent_dir) {
            dir_close(searched_record.parent_dir);
        }
        printk("exceed max open files\n");
        return -1;
    }

    // 填写文件结构
    file_table[fd_idx].fd_inode = inode_open(part, inode_no);
    file_table[fd_idx].fd_pos = 0;

    // 安装到进程 PCB
    int32_t ret_fd = pcb_fd_install(fd_idx);

    // 释放搜索过程中打开的父目录 inode (如果有的话)
    if (searched_record.parent_dir) {
        dir_close(searched_record.parent_dir);
    }
    
    return ret_fd;
}

int32_t sys_closedir(int32_t fd) {
    // 基础检查
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }

    // 找到对应的全局文件表项
    int32_t global_fd = fd_local2global(fd);
    struct file* f = &file_table[global_fd];

    if (f->fd_inode == NULL) {
        return -1;
    }

    // 嵌入 dir_close 函数的逻辑，判断是否为根目录
    // 根目录的 inode 是常驻内存的，不应该被关闭/释放
    if (f->fd_inode != root_dir.inode) { 
        inode_close(f->fd_inode);
    }

    // 清理资源，释放全局文件表槽位和进程 fd 槽位
    f->fd_inode = NULL;
    f->fd_pos = 0;
    f->fd_flag = 0;

    struct task_struct* cur = get_running_task_struct();
    cur->fd_table[fd] = -1;

    return 0;
}


int32_t sys_readdir(int32_t fd, struct dir_entry* de) {
    // 基础检查
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) return -1;

    int32_t _fd = fd_local2global(fd);
    struct file* f = &file_table[_fd];

    if (f->fd_inode == NULL || f->fd_inode->di.i_type != FT_DIRECTORY) {
        return -1;
    }

    // 用于拷贝目录项
	// tmp_dir.dir_buf 不需要初始化，dir_read 会负责填充它
    struct dir tmp_dir;
    tmp_dir.inode = f->fd_inode;   
    tmp_dir.dir_pos = f->fd_pos;   
    
    // 读取内核级的 dir_entry
    struct dir_entry* de_kernel = dir_read(&tmp_dir);

    if (de_kernel != NULL) {
        // 将数据安全拷贝到用户空间
        // 暂时先用 memcpy ，以后需要添加一个专门讲内核数据拷贝到用户数据的函数
        memcpy(de, de_kernel, sizeof(struct dir_entry));
        
        // 更新全局文件表的偏移量
        f->fd_pos = tmp_dir.dir_pos; 
        return 1;
    }
    
    return 0;
}

void sys_rewinddir(int32_t fd) {
    // 基础检查
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        printk("sys_rewinddir: fd %d is invalid\n", fd);
        return;
    }

    // 找到全局文件项
    int32_t global_fd = fd_local2global(fd);
    struct file* f = &file_table[global_fd];

    if (f->fd_inode->di.i_type != FT_DIRECTORY) {
        printk("sys_rewinddir: fd %d is not a directory (flag: %d)\n", fd, f->fd_flag);
        return;
    }

    // 重置偏移量
    f->fd_pos = 0;
}

int32_t sys_rmdir(const char* pathname){
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int inode_no = search_file(pathname,&searched_record);
	ASSERT(inode_no!=0);
	struct partition* part = get_part_by_rdev(searched_record.i_dev);
	int retval = -1;
	if(inode_no==-1){
		printk("In %s, sub path %s not exist\n",pathname,searched_record.searched_path);
	}else{
		if(searched_record.file_type==FT_REGULAR){
			printk("%s is regular file!\n",pathname);
		}else{
			
			struct dir* dir = dir_open(part,inode_no);
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

static uint32_t get_parent_dir_inode_nr(struct partition* part, uint32_t child_inode_nr,void* io_buf){
	struct m_inode* child_dir_inode = inode_open(part,child_inode_nr);
	uint32_t block_lba = child_dir_inode->di.i_sectors[0];
	ASSERT(block_lba>=part->sb->data_start_lba);
	inode_close(child_dir_inode);
	bread_multi(part->my_disk,block_lba,io_buf,1);
	struct dir_entry* dir_e = (struct dir_entry*)io_buf;

	ASSERT(dir_e[1].i_no<MAX_FILES_PER_PART&&dir_e[1].f_type==FT_DIRECTORY);
	return dir_e[1].i_no;
}

static int get_child_dir_name(struct partition* part, uint32_t p_inode_nr,uint32_t c_inode_nr,char* path,void* io_buf){
	struct m_inode* parent_dir_inode = inode_open(part,p_inode_nr);
	uint8_t block_idx = 0;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0},block_cnt = DIRECT_INDEX_BLOCK;
	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = parent_dir_inode->di.i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(parent_dir_inode->di.i_sectors[tfflib]){
		bread_multi(part->my_disk,parent_dir_inode->di.i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;
	}
	inode_close(parent_dir_inode);

	struct dir_entry* dir_e = (struct dir_entry*) io_buf;
	uint32_t dir_entry_size = part->sb->dir_entry_size;
	uint32_t dir_entrys_per_sec =(SECTOR_SIZE/dir_entry_size);

	block_idx = 0;

	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]){
			bread_multi(part->my_disk,all_blocks_addr[block_idx],io_buf,1);
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
	void* io_buf = kmalloc(SECTOR_SIZE);
	if(io_buf==NULL){
		return NULL;
	}
	
	struct task_struct* cur_thread = get_running_task_struct();
	int32_t parent_inode_nr = 0;
	int32_t child_inode_nr = cur_thread->cwd_inode_nr;
	ASSERT(child_inode_nr>=0&&child_inode_nr<MAX_FILES_PER_PART);

    struct m_inode* temp_inode = inode_open(cur_part, cur_thread->cwd_inode_nr); 
    struct partition* part = get_part_by_rdev(temp_inode->i_dev);
    inode_close(temp_inode);

	
	
	if(child_inode_nr==0){
		buf[0] = '/';
		buf[1] = 0;
		kfree(io_buf);
		return buf;
	}

	memset(buf,0,size);
	char full_path_reverse[MAX_PATH_LEN] = {0};
	
	while ((child_inode_nr)){
		parent_inode_nr = get_parent_dir_inode_nr(part,child_inode_nr,io_buf);
		if(get_child_dir_name(part,parent_inode_nr,child_inode_nr,full_path_reverse,io_buf)==-1){
			kfree(io_buf);
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
	kfree(io_buf);
	
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
		buf->st_size = root_dir.inode->di.i_size;
		return 0;
	}

	int32_t ret = -1;
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	int32_t inode_no = search_file(path,&searched_record);
	struct partition* part = get_part_by_rdev(searched_record.i_dev);

	if(inode_no!=-1){
		struct m_inode* obj_inode = inode_open(part,inode_no); 
		buf->st_size = obj_inode->di.i_size;
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
	char** granularits = kmalloc(sizeof(char*)*disk_num);
	uint8_t* div_cnts = kmalloc(sizeof(uint8_t)*disk_num);
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
					uint8_t* buf_bitmap_bits = kmalloc(bitmap_sects*SECTOR_SIZE);
					memset(buf_bitmap_bits,0,bitmap_sects*SECTOR_SIZE);

					bread_multi(&channels[channel_idx].devices[device_idx]
						,channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->block_bitmap_lba
						,buf_bitmap_bits
						,bitmap_sects
					);
					
					struct bitmap bitmap_buf;
					bitmap_buf.bits = buf_bitmap_bits;
					bitmap_buf.btmp_bytes_len = (channels[channel_idx].devices[device_idx].prim_parts[part_idx].sb->sec_cnt)/8;
					uint32_t free_sects = bitmap_count(&bitmap_buf);
					uint32_t used_sects = (bitmap_buf.btmp_bytes_len*8)-free_sects;
					kfree(buf_bitmap_bits);
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
					uint8_t* buf_bitmap_bits = kmalloc(bitmap_sects*SECTOR_SIZE);
					memset(buf_bitmap_bits,0,bitmap_sects*SECTOR_SIZE);

					bread_multi(&channels[channel_idx].devices[device_idx]
						,channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->block_bitmap_lba
						,buf_bitmap_bits
						,bitmap_sects
					);
					
					struct bitmap bitmap_buf;
					bitmap_buf.bits = buf_bitmap_bits;
					bitmap_buf.btmp_bytes_len = (channels[channel_idx].devices[device_idx].logic_parts[part_idx].sb->sec_cnt)/8;
					uint32_t free_sects = bitmap_count(&bitmap_buf);
					uint32_t used_sects = (bitmap_buf.btmp_bytes_len*8)-free_sects;
					kfree(buf_bitmap_bits);
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

static bool check_disk_name(struct dlist_elem* pelem,void* arg){
	char* part_name = (char*) arg;
	struct partition* part = member_to_entry(struct partition,part_tag,pelem);
	if(!strcmp(part_name,part->name)){
		return true;
	}
	return false;
}

void sys_mount(const char* part_name){
	// printk("mount::before:::%x \n",cur_part->sb->data_start_lba);
	struct dlist_elem* res = dlist_traversal(&partition_list,check_disk_name,(void*)part_name);
	if(res==NULL){
		printk("sys_mount: partition %s not found!\n",part_name);
		return;
	}

	if(cur_part!=NULL&&cur_part->block_bitmap.bits!=NULL){
		// printk("remove block bitmap\n");
		kfree(cur_part->block_bitmap.bits);
		cur_part->block_bitmap.btmp_bytes_len = 0;
	}

	if(cur_part!=NULL&&cur_part->inode_bitmap.bits!=NULL){
		// printk("remove inode bitmap\n");
		kfree(cur_part->inode_bitmap.bits);
		cur_part->inode_bitmap.btmp_bytes_len = 0;
	}

	if(cur_part!=NULL){
		close_root_dir(cur_part);
		printk("close root directory\n");
	} 

	dlist_traversal(&partition_list,mount_partition,(void*)part_name);
	open_root_dir(cur_part);
	// printk("mount::after:::%x \n",cur_part->sb->data_start_lba);
	// printk("partition %s mounted\n", cur_part->name);


	// 同步全局 cur_dir 指向新的 root_dir 内存
    cur_dir = &root_dir; 

    // 强制让当前运行进程的路径回到根目录，防止它引用旧分区的 inode 编号
    struct task_struct* cur = get_running_task_struct();
    cur->cwd_inode_nr = cur_part->sb->root_inode_no;

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

// 创建特殊文件（字符/块设备）
int32_t sys_mknod(const char* pathname, enum file_types type, uint32_t dev) {
    if (type != FT_CHAR_SPECIAL && type != FT_BLOCK_SPECIAL) {
        printk("sys_mknod: only support special files\n");
        return -1;
    }

    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) return -1;

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    int inode_no = search_file(pathname, &searched_record);

    // 检查目标是否已存在
    if (inode_no != -1) {
        printk("sys_mknod: file %s exists! ino:%d, type:%d, dev:0x%x\n", 
                pathname, inode_no, searched_record.file_type, searched_record.i_dev);
        goto rollback_path;
    }

    // 检查父目录是否存在（防止创建到不存在的路径下）
    if (path_depth_cnt((char*)pathname) != path_depth_cnt(searched_record.searched_path)) {
        printk("sys_mknod: parent dir not exist\n");
        goto rollback_path;
    }

    struct dir* parent_dir = searched_record.parent_dir;
    struct partition* part = get_part_by_rdev(searched_record.i_dev);
    char* filename = strrchr(searched_record.searched_path, '/') + 1;

    // 分配 Inode 编号
    inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        printk("sys_mknod: allocate inode failed\n");
        goto rollback_path;
    }

    // 初始化特殊的设备 Inode
    // 设备文件不占用磁盘数据块（i_sectors 全 0），其设备号存在 i_rdev 中
	// 因此其不用申请空闲磁盘块
    struct m_inode new_inode;
    inode_init(part, inode_no, &new_inode, type);
    new_inode.di.i_rdev = dev; 
    new_inode.di.i_size = 0;   // 特殊文件大小通常为 0

    // 同步新 Inode 到磁盘
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, &new_inode, io_buf);
    
    // 同步 Inode 位图到磁盘
    bitmap_sync(part, inode_no, INODE_BITMAP);

    // 创建并同步目录项到父目录的数据块中
    struct dir_entry new_de;
    memset(&new_de, 0, sizeof(struct dir_entry));
    create_dir_entry(filename, inode_no, type, &new_de);

    if (!sync_dir_entry(parent_dir, &new_de, io_buf)) {
        printk("sys_mknod: sync dir_entry failed\n");
        while(1);
        goto rollback_path;
    }

    // 同步父目录的 Inode
    // sync_dir_entry 会增加 parent_dir->inode->i_size
    // 如果不执行下面这行，磁盘上的 i_size 不变，ls 就读不到新项
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, parent_dir->inode, io_buf); 

    kfree(io_buf);
    dir_close(searched_record.parent_dir);
    printk("sys_mknod: %s created done!\n", pathname);
    return 0;

rollback_path:
    kfree(io_buf);
    if (searched_record.parent_dir) {
        dir_close(searched_record.parent_dir);
    }
    return -1;
}

// 复制文件描述符，为了之后文件重定向做准备
// BusyBox 的 sh 在执行命令前，会通过 dup2 把文件的 fd 强制覆盖到 stdout（fd 1）上。
// 如果没有这个系统调用，我们的 Shell 将无法实现输出重定向，BusyBox 里的很多脚本也跑不起来。
// dup2 的逻辑其实非常简单：让两个局部 FD 指向同一个全局文件项。
int32_t sys_dup2(uint32_t old_local_fd, uint32_t new_local_fd) {
    struct task_struct* cur = get_running_task_struct();

    // 基础校验
    if (old_local_fd >= MAX_FILES_OPEN_PER_PROC || cur->fd_table[old_local_fd] == -1) {
        return -1;
    }
    if (new_local_fd >= MAX_FILES_OPEN_PER_PROC) return -1;

	// 如果两个 FD 一样，按照 POSIX 标准直接返回
    if (old_local_fd == new_local_fd) return new_local_fd;
	

    // 如果 new_fd 已经指向某个文件，先关闭它
    if (cur->fd_table[new_local_fd] != -1) {
        sys_close(new_local_fd);
    }
	

	// 将两个局部描述符指向同一个全局描述符
    uint32_t global_fd = cur->fd_table[old_local_fd];
    cur->fd_table[new_local_fd] = global_fd;

	struct file* f = &file_table[global_fd];

	// 我们知道，dup2是让两个局部表项指向同一个全局表项
	// 因此此处是 f_count 增加
	file_table[global_fd].f_count++;

	// 管道特有逻辑, 增加端点计数
    // 如果不加这个，执行 dup2(pd[1], 1) 后 close(pd[1])，
    // pipe_release 会以为写端全关了，导致读者收到 EOF，但实际上 stdout 还在指向写端。
    if (f->fd_inode->di.i_type == FT_PIPE) {
        struct pipe* p = (struct pipe*)f->fd_inode->di.i_pipe_ptr;
        if (f->fd_flag & O_RDONLY) p->reader_count++;
        if (f->fd_flag & O_WRONLY) p->writer_count++;
    }

    return new_local_fd;
}

// 遍历分区链表的回调函数，用于创建设备节点
static bool make_partition_node(struct dlist_elem* pelem, void* arg UNUSED) {
    struct partition* part = member_to_entry(struct partition, part_tag, pelem);
    
    char dev_path[32] = {0};
    sprintf(dev_path, "/dev/%s", part->name);

    // 创建块设备节点
    // part->i_rdev 是我们在 partition_scan 中计算好的逻辑设备号
    if (sys_mknod(dev_path, FT_BLOCK_SPECIAL, part->i_rdev) == 0) {
        printk("  node /dev/%s created (dev_no:0x%x)\n", part->name, part->i_rdev);
    }
	
    
    return false; // 返回 false 以便继续遍历
}

// /dev 目录下的所有非目录文件（设备节点）
static void clear_dev_directory(void) {
    int32_t fd;
    struct dir_entry de;
    char path[MAX_PATH_LEN];
    bool deleted;

    do {
        deleted = false;
        fd = sys_opendir("/dev");
        if (fd == -1) {
            sys_mkdir("/dev");
            return;
        }

        while (sys_readdir(fd, &de) > 0) {
            if (!strcmp(de.filename, ".") || !strcmp(de.filename, "..")) {
                continue;
            }

            if (de.f_type != FT_DIRECTORY) {
                memset(path, 0, MAX_PATH_LEN);
                sprintf(path, "/dev/%s", de.filename);
				printk("clear_dev: delete file: %s\n",path);
                
                if (sys_unlink(path) == 0) {
                    deleted = true; 
                    // 删除了一个，为了保险，跳出内循环重新打开目录/重置指针
                    // 这样可以确保 i_size 的变化不会导致遍历跳过文件
                    break; 
                }
            }
        }
        sys_closedir(fd);
    } while (deleted); // 只要这一轮有删除动作，就再检查一遍

    printk("/dev directory cleared.\n");
}

// 自动化创建所有磁盘设备节点和其他设备节点
void make_dev_nodes(void) {
    // 彻底清空旧节点
	// 需要注意的一个问题是目前我们的clear_dev_directory里面只会删除/dev下的文件
	// 没有做到递归的删除 /dev 子目录下的文件
	// 因此我们现在的 /dev 目录还是扁平化管理的，即其下没有二级目录
    clear_dev_directory();
	
    // 创建核心字符设备 (控制台和终端)
    sys_mknod("/dev/console", FT_CHAR_SPECIAL, MAKEDEV(CONSOLE_MAJOR, 0));
    sys_mknod("/dev/tty0", FT_CHAR_SPECIAL, MAKEDEV(TTY_MAJOR, 0));

    // 动态创建磁盘母盘节点 (sda, sdb...)
	uint8_t c,d;
    for (c = 0; c < channel_cnt; c++) {
        for (d = 0; d < DISK_NUM_IN_CHANNEL; d++) {
            struct disk* hd = &channels[c].devices[d];
            if (hd->my_channel != NULL && hd->dev_no < 2) { 
                char dev_name[32];
                sprintf(dev_name,"/dev/%s", hd->name);
                sys_mknod(dev_name, FT_BLOCK_SPECIAL, hd->i_rdev);
				printk("/dev/%s rdev: %x\n",hd->name,hd->i_rdev);
            }
        }
    }

    // 遍历分区链表，创建分区节点 (sda1, sda5...)
    dlist_traversal(&partition_list, make_partition_node, NULL);

    printk("/dev nodes dynamic creation done.\n");
}