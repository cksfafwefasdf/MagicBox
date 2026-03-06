#include <fs.h>
#include <sifs_dir.h>
#include <debug.h>
#include <string.h>
#include <stdio-kernel.h>
#include <stdbool.h>
#include <dlist.h>
#include <console.h>
#include <thread.h>
#include <ioqueue.h>
#include <keyboard.h>
#include <pipe.h>
#include <bitmap.h>
#include <memory.h>
#include <tty.h>
#include <device.h>
#include <fs_types.h>
#include <stdio.h>
#include <ide.h>
#include <sifs_file.h>
#include <sifs_inode.h>
#include <ide_buffer.h>
#include <interrupt.h>
#include <fifo.h>
#include <file_table.h>
#include <sifs_sb.h>
#include <inode.h>


struct partition* cur_part;
struct inode* cur_dir_inode;

static bool mount_partition(struct dlist_elem* pelem, void* arg) {
    char* part_name = (char*)arg;
    struct partition* part = member_to_entry(struct partition, part_tag, pelem);
    
    if (strcmp(part->name, part_name) != 0) return false;

    // 申请 VFS 级别的超级块内存
    struct super_block* sb = kmalloc(sizeof(struct super_block));
    if (!sb) PANIC("VFS: kmalloc super_block failed!");
    memset(sb, 0, sizeof(struct super_block));

    // 设置通用字段
    sb->s_dev = part->i_rdev;

    // 根据文件系统类型进行分发 (Dispatch)
    // 暂时我们可以根据魔数探测，或者手动指定
    // 之后我们实现虚拟文件系统后可以直接用VFS的多态方式来读取
    if (sifs_read_super(sb, NULL, 1) != NULL) {
        // 挂载成功，完成最后的绑定
        cur_part = part;
        printk("VFS: mounted %s as SIFS\n", part_name);
        return true;
    } 
    /* else if (ext2_read_super(sb, NULL, 1) != NULL) { ... } */
    
    // 如果所有文件系统都识别失败
    kfree(sb);
    printk("VFS: can't find valid filesystem on %s\n", part_name);
    return false;
}

void filesys_init() {

    inode_cache_init();

    uint8_t channel_no = 0, dev_no = 0;
    bool first_flag = true;
    char default_part[MAX_DISK_NAME_LEN] = {0};

    printk("Searching filesystem......\n");
    while (channel_no < channel_cnt) {
        dev_no = 0;
        while (dev_no < 2) {
            struct disk* hd = &channels[channel_no].devices[dev_no];
            if (!hd->name[0]) { dev_no++; continue; } 

            struct partition* part = hd->prim_parts;
            uint8_t part_idx = 0;
            while (part_idx < 12) {
                if (part_idx == 4) part = hd->logic_parts;

                if (part->sec_cnt != 0) {
                    // 临时申请一个磁盘超级块缓冲区，仅用于探测
                    struct sifs_super_block* sb_buf = (struct sifs_super_block*)kmalloc(SECTOR_SIZE);
                    if (sb_buf == NULL) PANIC("filesys_init: kmalloc failed!");
                    
                    bread_multi(hd, part->start_lba + 1, sb_buf, 1);

                    // 检查魔数，如果没有则格式化
                    if (sb_buf->magic != SIFS_FS_MAGIC_NUMBER) {
                        printk("Formatting %s's partition %s ......\n", hd->name, part->name);
                        sifs_format(part);
                    }
                    
                    // 探测完毕，释放临时缓冲区
                    kfree(sb_buf);

                    // 记录第一个有效分区的名字，准备挂载
                    if (first_flag) {
                        strcpy(default_part, part->name);
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

    // 调用 mount_partition 进行挂载
    // mount_partition 内部会调用 sifs_read_super，加载超级块
    // 而 sifs_read_super 会完成分配 VFS super_block、位图、根 inode 的所有工作
    if (default_part[0] != 0) {
        dlist_traversal(&partition_list, mount_partition, (void*)default_part);
    } else {
        PANIC("No available partition to mount!");
    }

    // 设置当前目录环境
    // sifs_read_super 已经把 root_inode 存进了 cur_part->sb->s_root_inode
    cur_dir_inode = cur_part->sb->s_root_inode; 
    open_root_dir(cur_part);
    // 初始化全局文件表
    uint32_t fd_idx = 0;
    while (fd_idx < MAX_FILE_OPEN_IN_SYSTEM) {
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
static int32_t search_file(const char* pathname, struct path_search_record* searched_record) {
    // 处理根目录特例
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/..")) {
        // 由于 root_dir 是全局 file，其 inode 应该始终打开
        searched_record->parent_inode = inode_open(cur_part, root_dir_inode->i_no); 
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0;
        searched_record->i_dev = root_dir_inode->i_dev;
        return root_dir_inode->i_no;
    }

    uint32_t path_len = strlen(pathname);
    ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);
    
    char* sub_path = (char*)pathname;
    // 初始起点，直接使用全局根目录的 inode
    struct inode* parent_inode = inode_open(cur_part, root_dir_inode->i_no);
    // struct inode* parent_inode = root_dir.fd_inode; 

    struct sifs_dir_entry dir_e; // 使用磁盘镜像结构
    char name[MAX_FILE_NAME_LEN] = {0};

    searched_record->parent_inode = parent_inode;
    searched_record->file_type = FT_UNKNOWN;
    searched_record->i_dev = parent_inode->i_dev;

    uint32_t parent_inode_no = parent_inode->i_no; 
    struct partition* part;

    sub_path = _path_parse(sub_path, name);

    while (name[0]) {
        ASSERT(strlen(searched_record->searched_path) < 512);
        part = get_part_by_rdev(searched_record->i_dev);

        // 记录已经扫描过的路径
        strcat(searched_record->searched_path, "/");
        strcat(searched_record->searched_path, name);

        // 在当前 inode 指向的目录中查找该名字
        if (sifs_search_dir_entry(part, parent_inode, name, &dir_e)) {
            memset(name, 0, MAX_FILE_NAME_LEN);
            // console_put_str(sub_path);console_put_str("\n");
            if (sub_path) {
                sub_path = _path_parse(sub_path, name);
            }

            if (FT_DIRECTORY == dir_e.f_type) {
                // 记录当前目录作为“下一轮的父目录”
                parent_inode_no = parent_inode->i_no;

                // 如果当前目录不是根目录，需要释放其引用计数（因为它即将被替换）
                if (parent_inode != root_dir_inode) {
                    inode_close(parent_inode);
                }

                // 进入下一级目录
                parent_inode = inode_open(part, dir_e.i_no);
                searched_record->parent_inode = parent_inode;
                searched_record->i_dev = parent_inode->i_dev;
                continue;
            } else {
                // 找到了目标文件（普通文件等）
                searched_record->file_type = dir_e.f_type;
                return dir_e.i_no;
            }
        } else {
            // 找不到该名字
            return -1;
        }
    }

    // 路径以 "/" 结尾的情况处理 (例如 "/dev/")
    // 为了符合逻辑，parent_inode 应当回退到该目录的父目录
    part = get_part_by_rdev(searched_record->parent_inode->i_dev);
    
    // 关闭当前指向的目录，重新打开它的父目录
    if (searched_record->parent_inode != root_dir_inode) {
        inode_close(searched_record->parent_inode);
    }
    searched_record->parent_inode = inode_open(part, parent_inode_no);
    
    searched_record->file_type = FT_DIRECTORY;
    return dir_e.i_no;
}

// 将路径转换为绝对路径
static void make_abs_pathname(const char* pathname, char* abs_path) {
    if (pathname[0] == '/') {
        // 如果已经是绝对路径，直接拷贝
        strcpy(abs_path, pathname);
    } else {
        // 如果是相对路径，先获取当前目录
        sys_getcwd(abs_path, MAX_PATH_LEN);
        
        // 针对根目录 "/" 做特殊处理，避免拼成 "//name"
        if (strcmp(abs_path, "/") != 0) {
            strcat(abs_path, "/");
        }
        strcat(abs_path, pathname);
    }
}

int32_t sys_open(const char* _pathname,uint8_t flags){
	// printk("sys_open:::pathname: %s\n",pathname);

	char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); // 统一转成绝对路径

	// ASSERT(flags<=7);
	int32_t fd = -1;

	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	
	uint32_t pathname_depth = path_depth_cnt((char*)pathname);
	
	int inode_no = search_file(pathname,&searched_record);
	bool found = inode_no !=-1?true:false;

	// 只有找到了文件，才去校验它是不是目录
	if (found && searched_record.file_type == FT_DIRECTORY) {
		// printk("can't open a directory with open()...\n");
		// inode_close(searched_record.parent_inode);
		// 如果是目录，检查 flags 是否合法
        // 目录不允许写入、截断或创建
        if (flags & (O_WRONLY | O_RDWR | O_CREATE)) {
            printk("sys_open: directory %s is read-only\n", pathname);
            inode_close(searched_record.parent_inode);
            return -1;
        }
        // 校验通过，允许进入下面的 file_open 流程
	}

	uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
	// to process the situation /a/b/c (/a/b is a regular file or not exists)
	if(pathname_depth!=path_searched_depth && strcmp(pathname, "/")){
		printk("sys_open: cannot access %s: Not a directory, subpath %s is't exist\n",\
		pathname,searched_record.searched_path);
		inode_close(searched_record.parent_inode);
		return -1;
	}

	if(!found&&!(flags&O_CREATE)){
		printk("in path %s,file %s is't exist\n",\
		searched_record.searched_path,\
		(strrchr(searched_record.searched_path,'/')+1));
		inode_close(searched_record.parent_inode);
		return -1;
	}else if(found && (flags & O_CREATE)){
		printk("%s has already exist!\n",pathname);
		inode_close(searched_record.parent_inode);
		return -1;
	}

	switch (flags&O_CREATE){
		case  O_CREATE:
			printk("creating file\n");
			fd = sifs_file_create(searched_record.parent_inode,(strrchr(pathname,'/')+1),flags);
			inode_close(searched_record.parent_inode);
			break;
		default: // file is existed, open the file
			// O_RDONLY,O_WRONLY,O_RDWRget_part_by_rdev
			struct partition* part = get_part_by_rdev(searched_record.i_dev);
			fd = file_open(part,inode_no,flags);
			inode_close(searched_record.parent_inode); // 关掉父文件夹，解析任务已经完成了
			break;
	}

	// 对于 FIFO 的处理 
	// 检查这个 FIFO 之前是否被打开过
    if (fd != -1) {
        struct task_struct* cur = get_running_task_struct();
        struct file* f = &file_table[cur->fd_table[fd]];
        struct inode* inode = f->fd_inode;

        if (inode->i_type == FT_FIFO) {
            int32_t ret = fifo_open(inode,f);
			if(ret == -1){
				PANIC("fail to init fifo inode.");
			}
        }
    }

    // printk("SYNC_CHECK: ino=%d, new_size=%d\n", searched_record.parent_inode->i_no, searched_record.parent_inode->i_size);

	return fd;
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
    if (file->fd_inode->i_type == FT_PIPE || file->fd_inode->i_type == FT_FIFO) {
        pipe_release(file);
    }

    // 调用通用的文件关闭函数，处理 f_count 和 inode_close
	// 对于 FIFO 和 PIPE，我们会在 inode_close 里面处理缓冲区的释放逻辑
	// 以便保证只要inode被销毁，相应的缓存区也一定被销毁，保证强一致性
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
    int32_t _fd = fd_local2global(get_running_task_struct(), fd);
    struct file* wr_file = &file_table[_fd];

    // 权限检查
    if (!(wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR)) {
        return -1;
    }

    struct inode* inode = wr_file->fd_inode;
    ASSERT(inode != NULL);

    // 根据 inode 类型进行分发
    enum file_types type = inode->i_type;

    switch (type) {
        case FT_PIPE:
		case FT_FIFO: // 具名管道和匿名管道在读写逻辑上是完全一样的
            // 管道逻辑
            return pipe_write(wr_file, buf, count);

        case FT_CHAR_SPECIAL:
            // 字符设备逻辑
            uint32_t major = MAJOR(inode->i_rdev);
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
    int32_t global_fd_idx = fd_local2global(get_running_task_struct(), fd);
    struct file* rd_file = &file_table[global_fd_idx];

    // 权限检查
    if (!(rd_file->fd_flag & O_RDONLY || rd_file->fd_flag & O_RDWR)) {
        return -1;
    }

    struct inode* inode = rd_file->fd_inode;
    ASSERT(inode != NULL);

    // 根据 Inode 类型进行统一分发
    enum file_types type = inode->i_type;

    switch (type) {
        case FT_PIPE:
		case FT_FIFO: // 具名管道和匿名管道在读写逻辑上是完全一样的
            // 管道逻辑
            return pipe_read(rd_file, buf, count);

        case FT_CHAR_SPECIAL: {
            // 字符设备分发
            uint32_t major = MAJOR(inode->i_rdev);
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
	uint32_t _fd = fd_local2global(get_running_task_struct(), fd);
	struct file* pf = &file_table[_fd];

	int32_t new_pos = 0;
	int32_t file_size = (int32_t)pf->fd_inode->i_size;
	enum file_types type = pf->fd_inode->i_type;

	// 先计算指针位置
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

	// 根据计算出的结果进行边界检查
	if (type == FT_REGULAR || type == FT_DIRECTORY) {
		// 普通文件和目录：严格受 i_size 限制
		if (new_pos < 0 || (uint32_t)new_pos > pf->fd_inode->i_size) {
			return -1;
		}
	} else if (type == FT_BLOCK_SPECIAL) {
		// 块设备, 检查是否超过分区物理边界
		struct partition* part = get_part_by_rdev(pf->fd_inode->i_rdev);
		uint32_t part_size = part->sec_cnt * SECTOR_SIZE;
		if (new_pos < 0 || (uint32_t)new_pos >= part_size) {
			return -1;
		}
	} else {
		// 字符设备、管道等 通常不支持 lseek，或者逻辑不同
		printk("sys_lseek: this type of file/device doesn't support lseek!\n");
		return -1;
	}
	
	pf->fd_pos = new_pos;
	return pf->fd_pos;
}

int32_t sys_unlink(const char* _pathname) {
    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); // 统一转成绝对路径

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 查找文件
    int inode_no = search_file(pathname, &searched_record);
    
    // 根目录不允许 unlink (inode 0 通常是根目录)
    ASSERT(inode_no != 0);
    
    if (inode_no == -1) {
        printk("file %s not found!\n", pathname);
        // 如果 search_file 失败，内部可能已经打开了 parent_inode，需要释放
        if (searched_record.parent_inode && searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        return -1;
    }

    // 检查文件类型，unlink 只能删除非目录文件
    if (searched_record.file_type == FT_DIRECTORY) {
        printk("can't delete a directory with unlink(), use rmdir() instead!\n");
        if (searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        return -1;
    }

    // 检查文件是否正在被系统打开
    // 我们通常不允许删除已打开的文件
    uint32_t file_idx = 0;
    while (file_idx < MAX_FILE_OPEN_IN_SYSTEM) {
        if (file_table[file_idx].fd_inode != NULL && (uint32_t)inode_no == file_table[file_idx].fd_inode->i_no) {
            break;
        }
        file_idx++;
    }

    if (file_idx < MAX_FILE_OPEN_IN_SYSTEM) {
        if (searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        printk("file %s is in use, not allowed to delete!\n", pathname);
        return -1;
    }

    // 执行删除逻辑
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) {
        if (searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        printk("sys_unlink: malloc for io_buf failed!\n");
        return -1;
    }

    struct inode* parent_inode = searched_record.parent_inode;
    struct partition* part = get_part_by_rdev(parent_inode->i_dev);

    // 从父目录中删除目录项 (使用重构后的 inode 版函数)
    if (sifs_delete_dir_entry(part, parent_inode, inode_no, io_buf)) {
        // 彻底释放文件的 inode 及其占用的磁盘块
        inode_release(part, inode_no);
    } else {
        printk("sys_unlink: delete_dir_entry failed!\n");
        kfree(io_buf);
        if (searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        return -1;
    }

    kfree(io_buf);
    
    // 清理 search_file 留在内存里的父目录句柄
    if (searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }
    
    return 0;
}

// mkdir 和 rmdir 是放到 i_op 中的成员，他就是和文件系统强相关的，所以直接写sifs没问题
// 直接操作 sifs 的超级块信息也没啥问题
int32_t sys_mkdir(const char* _pathname) {
    uint8_t rollback_step = 0;
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) {
        printk("sys_mkdir: kmalloc for io_buf failed!\n");
        return -1;
    }

    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); 

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 搜索路径
    int32_t inode_no = search_file(pathname, &searched_record);

    // 校验目录是否已存在
    if (inode_no != -1) {
        printk("sys_mkdir: file or directory %s already exists!\n", pathname);
        rollback_step = 1; // 需要释放 parent_inode
        goto rollback;
    } else {
        // 校验父目录是否存在
        uint32_t pathname_depth = path_depth_cnt((char*)pathname);
        uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
        if (pathname_depth != path_searched_depth) {
            printk("sys_mkdir: subpath %s does not exist\n", searched_record.searched_path);
            rollback_step = 1;
            goto rollback;
        }
    }

    struct inode* parent_inode = searched_record.parent_inode;
    // 获取待创建的目录名（最后一级路径）
    char* dir_name = strrchr(pathname, '/') + 1;
    struct partition* part = get_part_by_rdev(parent_inode->i_dev);

    // 分配 Inode
    inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        printk("sys_mkdir: allocate inode failed\n");
        rollback_step = 1;
        goto rollback;
    }

    // 初始化内存中的新 Inode (局部变量即可，因为稍后会同步到磁盘)
    struct inode new_dir_inode;
    inode_init(part, inode_no, &new_dir_inode, FT_DIRECTORY);

    // 为新目录分配第一个数据块，用于存储 . 和 ..
    int32_t block_lba = block_bitmap_alloc(part);
    if (block_lba == -1) {
        printk("sys_mkdir: block_bitmap_alloc failed!\n");
        rollback_step = 2; // 需要回滚 inode 位图
        goto rollback;
    }
    new_dir_inode.sifs_i.i_sectors[0] = block_lba;

    // 初始化新目录的数据块 (写入 . 和 ..)
    memset(io_buf, 0, SECTOR_SIZE * 2);
    struct sifs_dir_entry* p_de = (struct sifs_dir_entry*)io_buf;

    // 初始化 "."
    memcpy(p_de->filename, ".", 1);
    p_de->i_no = inode_no;
    p_de->f_type = FT_DIRECTORY;

    // 初始化 ".."
    p_de++;
    memcpy(p_de->filename, "..", 2);
    p_de->i_no = parent_inode->i_no; // 指向父目录 inode
    p_de->f_type = FT_DIRECTORY;

    // 写入物理磁盘
    bwrite_multi(part->my_disk, block_lba, io_buf, 1);

    new_dir_inode.i_size = 2 * part->sb->sifs_info.sb_raw.dir_entry_size;

    // 在父目录中同步新目录的目录项
    struct sifs_dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct sifs_dir_entry));
    sifs_create_dir_entry(dir_name, inode_no, FT_DIRECTORY, &new_dir_entry);

    memset(io_buf, 0, SECTOR_SIZE * 2);
    if (!sifs_sync_dir_entry(parent_inode, &new_dir_entry, io_buf)) {
        printk("sys_mkdir: sifs_sync_dir_entry failed!\n");
        rollback_step = 3; // 需要回滚 block 位图和 inode 位图
        goto rollback;
    }

    //同步所有元数据到磁盘
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, parent_inode, io_buf); // 同步父目录（i_size 变了）

    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, &new_dir_inode, io_buf); // 同步新目录 inode

    bitmap_sync(part, inode_no, INODE_BITMAP);
    bitmap_sync(part, (block_lba - part->sb->sifs_info.sb_raw.data_start_lba), BLOCK_BITMAP);

    // 清理并退出
    kfree(io_buf);
    if (parent_inode != root_dir_inode) {
        inode_close(parent_inode);
    }

    return 0;

rollback:
    switch (rollback_step) {
        case 3:
            // 此时 block 已分配但同步目录项失败，需回滚 block
            bitmap_set(&part->sb->sifs_info.block_bitmap, (block_lba - part->sb->sifs_info.sb_raw.data_start_lba), 0);
        case 2:
            bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        case 1:
            if (searched_record.parent_inode != root_dir_inode) {
                inode_close(searched_record.parent_inode);
            }
            break;
    }
    kfree(io_buf);
    return -1;
}

int32_t sys_readdir(int32_t fd, struct dirent* de) {

    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return -1;
    }

    int32_t _fd = fd_local2global(get_running_task_struct(), fd);
    struct file* f = &file_table[_fd];

    // 类型检查，必须是目录且已打开
    if (f->fd_inode == NULL || f->fd_inode->i_type != FT_DIRECTORY) {
        printk("sys_readdir: fd %d is not a directory or not opened!\n", fd);
        return -1;
    }


    // sifs_dir_read 内部会处理 fd_pos 增加和 dirent 填充
    int status = sifs_dir_read(f, de);

    if (status == 0) {
        // 成功读取到一个有效条目
        return 1;
    } else {
        // status 为 -1，说明读到了目录末尾或发生错误
        // 在 sys 调用层面，读完通常返回 0，这是为了配合 while(readdir(...) > 0)
        return 0;
    }
}

void sys_rewinddir(int32_t fd) {
    // 基础检查
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        printk("sys_rewinddir: fd %d is invalid\n", fd);
        return;
    }

    // 找到全局文件项
    int32_t global_fd = fd_local2global(get_running_task_struct() ,fd);
    struct file* f = &file_table[global_fd];

    if (f->fd_inode->i_type != FT_DIRECTORY) {
        printk("sys_rewinddir: fd %d is not a directory (flag: %d)\n", fd, f->fd_flag);
        return;
    }

    // 重置偏移量
    f->fd_pos = 0;
}

int32_t sys_rmdir(const char* _pathname) {

    // 拦截 . 和 ..
    if (!strcmp(_pathname, ".") || !strcmp(_pathname, "..")) {
        printk("sys_rmdir: cannot remove '.' or '..'!\n");
        return -1;
    }

    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); // 统一转成绝对路径
    
    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 查找目标目录
    int inode_no = search_file(pathname, &searched_record);
    
    // 根目录不允许删除
    ASSERT(inode_no != 0);
    
    int retval = -1;
    struct partition* part = get_part_by_rdev(searched_record.i_dev);

    if (inode_no == -1) {
        printk("In %s, sub path %s not exist\n", pathname, searched_record.searched_path);
        // 即便没找到，如果 parent 不是根目录，也要释放 search_file 产生的引用
        goto exit;
    }

    // 检查类型
    if (searched_record.file_type == FT_REGULAR) {
        printk("%s is a regular file, use unlink() instead!\n", pathname);
        goto exit;
    }

    // 打开目标目录的 inode 以便检查其内容
    struct inode* target_inode = inode_open(part, inode_no);

    // 检查目录是否为空（只含有 . 和 ..）
    if (!sifs_dir_is_empty(target_inode)) {
        printk("dir %s is not empty, delete is not allowed!\n", pathname);
    } else {
        // 执行删除操作
        // 从父目录中删除该目录项
        void* io_buf = kmalloc(SECTOR_SIZE * 2);
        if (io_buf == NULL) {
            printk("sys_rmdir: kmalloc failed\n");
            inode_close(target_inode);
            goto exit;
        }

        if (sifs_delete_dir_entry(part, searched_record.parent_inode, inode_no, io_buf)) {
            // 释放目标目录占用的 inode 和数据块
            inode_release(part, inode_no);
            retval = 0;
        }
        kfree(io_buf);
    }

    // 关闭刚刚 open 的目标 inode
    inode_close(target_inode);

exit:
    //  清理, 关闭 search_file 打开的父目录 inode
    if (searched_record.parent_inode && searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }
    return retval;
}

static uint32_t get_parent_dir_inode_nr(struct partition* part, uint32_t child_inode_nr, void* io_buf) {
    // 打开当前目录的 inode 以获取其数据块地址
    struct inode* child_dir_inode = inode_open(part, child_inode_nr);
    
    // 目录的第 0 个块通常存放着 "." 和 ".."
    uint32_t block_lba = child_dir_inode->sifs_i.i_sectors[0];
    ASSERT(block_lba >= part->sb->sifs_info.sb_raw.data_start_lba);
    
    // 拿到 LBA 后就可以关闭 inode 了，节省内存引用计数
    inode_close(child_dir_inode);

    // 读取该块数据
    bread_multi(part->my_disk, block_lba, io_buf, 1);
    
    // 使用磁盘镜像结构体 sifs_dir_entry 而不是内存镜像的结构体
    struct sifs_dir_entry* dir_e = (struct sifs_dir_entry*)io_buf;

    // 根据我们在 sifs_format 中的约定：
    // dir_e[0] 是 "."
    // dir_e[1] 是 ".."
    
    // 确保它确实是一个目录类型
    ASSERT(dir_e[1].f_type == FT_DIRECTORY);
    ASSERT(dir_e[1].i_no < MAX_FILES_PER_PART);

    return dir_e[1].i_no;
}

static int get_child_dir_name(struct partition* part, uint32_t p_inode_nr, uint32_t c_inode_nr, char* path, void* io_buf) {
    // 打开父目录 Inode 以便遍历其目录项
    struct inode* parent_dir_inode = inode_open(part, p_inode_nr);
    
    uint8_t block_idx = 0;
    uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};
    uint32_t block_cnt = DIRECT_INDEX_BLOCK;

    // 填充直接块地址
    while (block_idx < DIRECT_INDEX_BLOCK) {
        all_blocks_addr[block_idx] = parent_dir_inode->sifs_i.i_sectors[block_idx];
        block_idx++;
    }

    // 处理一级间接索引块
    int tfflib = DIRECT_INDEX_BLOCK;
    if (parent_dir_inode->sifs_i.i_sectors[tfflib]) {
        bread_multi(part->my_disk, parent_dir_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
        block_cnt = TOTAL_BLOCK_COUNT;
    }

    // 拿到所有块地址后，可以提前关闭父目录 Inode
    inode_close(parent_dir_inode);

    // 使用 SIFS 磁盘目录项结构体
    struct sifs_dir_entry* dir_e = (struct sifs_dir_entry*)io_buf;
    uint32_t dir_entry_size = part->sb->sifs_info.sb_raw.dir_entry_size;
    uint32_t dir_entrys_per_sec = (SECTOR_SIZE / dir_entry_size);

    block_idx = 0;
    while (block_idx < block_cnt) {
        if (all_blocks_addr[block_idx]) {
            // 读取父目录的一个数据块
            bread_multi(part->my_disk, all_blocks_addr[block_idx], io_buf, 1);
            
            uint8_t dir_e_idx = 0;
            while (dir_e_idx < dir_entrys_per_sec) {
                // 匹配子目录的 Inode 编号
                if (dir_e[dir_e_idx].i_no == c_inode_nr) {
                    // 找到了匹配的条目，将名字拼接进路径缓冲区
                    strcat(path, "/");
                    strcat(path, dir_e[dir_e_idx].filename);
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

    struct inode* temp_inode = inode_open(cur_part, cur_thread->cwd_inode_nr); 
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

int32_t sys_chdir(const char* _pathname){
	int32_t ret = -1;
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));

	char path[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, path); // 统一转成绝对路径
	

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
	inode_close(searched_record.parent_inode);
	return ret;
}

int32_t sys_stat(const char* _pathname,struct stat* buf){

	char path[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, path); // 统一转成绝对路径

	if(!strcmp(path,"/")||!strcmp(path,"/.")||!strcmp(path,"/..")){
		buf->st_filetype = FT_DIRECTORY;
		buf->st_ino = 0;
		buf->st_size = root_dir_inode->i_size;
		return 0;
	}

	int32_t ret = -1;
	struct path_search_record searched_record;
	memset(&searched_record,0,sizeof(struct path_search_record));
	
	int32_t inode_no = search_file(path,&searched_record);
	struct partition* part = get_part_by_rdev(searched_record.i_dev);

	if(inode_no!=-1){
		struct inode* obj_inode = inode_open(part,inode_no); 
		buf->st_size = obj_inode->i_size;
		inode_close(obj_inode);
		buf->st_filetype = searched_record.file_type;
		buf->st_ino = inode_no;
		ret = 0;

	}else{	
		printk("sys_stat: %s not found!\n",path);
	}

	inode_close(searched_record.parent_inode);
	return ret;
}

// partition formatlize 512B-blocks used avaliable
void sys_disk_info() {
    uint8_t channel_idx;
    printk("disk number: %d\n", disk_num);

    // 处理磁盘容量单位
    char** granularits = kmalloc(sizeof(char*) * disk_num);
    uint8_t* div_cnts = kmalloc(sizeof(uint8_t) * disk_num);
    memset(div_cnts, 0, disk_num);

    for (int i = 0; i < disk_num; i++) {
        uint32_t temp_size = disk_size[i];
        while (temp_size > 1024) {
            temp_size /= 1024;
            div_cnts[i]++;
        }
        switch (div_cnts[i]) {
            case 0: granularits[i] = "B";  break;
            case 1: granularits[i] = "KB"; break;
            case 2: granularits[i] = "MB"; break;
            case 3: granularits[i] = "GB"; break;
            case 4: granularits[i] = "TB"; break;
            default: granularits[i] = "OVERFLOW!"; break;
        }
    }

    // 打印磁盘基本容量 (sda, sdb...)
    for (channel_idx = 0; channel_idx < CHANNEL_NUM; channel_idx++) {
        for (uint8_t ide_idx = 0; ide_idx < DEVICE_NUM_PER_CHANNEL; ide_idx++) {
            struct disk* hd = &channels[channel_idx].devices[ide_idx];
            if (!hd->name[0]) continue;
            uint8_t d_idx = channel_idx * DISK_NUM_IN_CHANNEL + ide_idx;
            // 这里用原始 disk_size 计算出的缩放值
            uint32_t display_size = disk_size[d_idx];
            for(int j=0; j<div_cnts[d_idx]; j++) display_size /= 1024;
            printk("%s\t%d%s\n", hd->name, display_size, granularits[d_idx]);
        }
    }

    printk("partition\tformatlize\t512B-blocks\tused\tavaliable\n");

    for (channel_idx = 0; channel_idx < CHANNEL_NUM; channel_idx++) {
        for (uint8_t device_idx = 0; device_idx < DEVICE_NUM_PER_CHANNEL; device_idx++) {
            struct disk* hd = &channels[channel_idx].devices[device_idx];
            if (!hd->name[0]) continue;

            // 检查是否有分区
            bool has_partition = false;
            for (uint8_t i = 0; i < PRIM_PARTS_NUM; i++) {
                if (hd->prim_parts[i].name[0]) { has_partition = true; break; }
            }

            if (has_partition) {
                // 分两轮打印：第一轮主分区(P)，第二轮逻辑分区(L)
                for (int type = 0; type < 2; type++) {
                    int limit = (type == 0) ? PRIM_PARTS_NUM : LOGIC_PARTS_NUM;
                    for (int p_idx = 0; p_idx < limit; p_idx++) {
                        struct partition* part = (type == 0) ? &hd->prim_parts[p_idx] : &hd->logic_parts[p_idx];
                        if (!part->name[0]) continue;

                        struct sifs_super_block* sifs_sb = &part->sb->sifs_info.sb_raw;
                        bool is_temp_sb = false;

                        // 即便没挂载，也现场读
                        if (part->sb != NULL) {
                            // 如果已经挂载，直接用内存里的
                            sifs_sb = &part->sb->sifs_info.sb_raw;
                        } else {
                            // 没挂载，现场申请并读取
                            sifs_sb = kmalloc(SECTOR_SIZE);
                            if (sifs_sb == NULL) PANIC("sys_disk_info: kmalloc failed");
                            
                            bread_multi(hd, part->start_lba + 1, sifs_sb, 1);
                            is_temp_sb = true;
                        }

                        // 如果魔数对不上，说明没格式化，打印横杠
                        if (sifs_sb->magic != SIFS_FS_MAGIC_NUMBER) { 
                            printk("%s(%c)\t-\t-\t-\t-\n", part->name, (type == 0 ? 'P' : 'L'));
                        } else {
                            // 计算位图
                            uint32_t btmp_sects = sifs_sb->block_bitmap_sects;
                            uint8_t* btmp_buf = kmalloc(btmp_sects * SECTOR_SIZE);
                            bread_multi(hd, sifs_sb->block_bitmap_lba, btmp_buf, btmp_sects);

                            struct bitmap btmp;
                            btmp.bits = btmp_buf;
                            btmp.btmp_bytes_len = sifs_sb->sec_cnt / 8;
                            uint32_t free_sects = bitmap_count(&btmp);
                            uint32_t used_sects = (btmp.btmp_bytes_len * 8) - free_sects;

                            char cur_flag_str[2] = {0};
                            if (cur_part != NULL && !strcmp(cur_part->name, part->name)) {
                                cur_flag_str[0] = '*';
                            }

                            printk("%s(%c)%s\t%x\t%d\t%d\t%d\n", 
                                   part->name, 
                                   (type == 0 ? 'P' : 'L'), 
                                   cur_flag_str, 
                                   sifs_sb->magic, 
                                   sifs_sb->sec_cnt, 
                                   used_sects, 
                                   free_sects);

                            kfree(btmp_buf);
                        }
                        if (is_temp_sb) kfree(sifs_sb);
                    }
                }
            } else {
                // R means raw disk
                uint8_t d_idx = channel_idx * DISK_NUM_IN_CHANNEL + device_idx;
                uint32_t display_size = disk_size[d_idx];
                for(int j=0; j<div_cnts[d_idx]; j++) display_size /= 1024;
                printk("%s(R)\t-\t-\t-\t%d%s\n", hd->name, display_size, granularits[d_idx]);
            }
        }
    }
    kfree(granularits);
    kfree(div_cnts);
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

	struct partition* part = member_to_entry(struct partition, part_tag, res);

	// 由于我们引入了全盘分区，因此需要区分现在挂载的到底是全盘分区还是正常的装有文件系统的分区
	// 若是全盘分区则直接拒绝
	// 临时申请一个缓冲区来读取潜在的超级块
	struct sifs_super_block* sb_buf = (struct sifs_super_block*)kmalloc(SECTOR_SIZE);
	
	// 超级块在第一个扇区，因此使用 part->start_lba + 1
	bread_multi(part->my_disk, part->start_lba + 1, sb_buf, 1);

	// 检查魔数是否匹配,即是否有文件系统
	if (sb_buf->magic != SIFS_FS_MAGIC_NUMBER) {
		printk("sys_mount: Error! Partition %s is not a valid filesystem (Magic Mismatch).\n", part_name);
		kfree(sb_buf);
		return ; // 找到了这个盘，但它不合法，直接返回 true 停止遍历
	}

    if(cur_part!=NULL){

        if(cur_part->sb->sifs_info.block_bitmap.bits!=NULL){
            kfree(cur_part->sb->sifs_info.block_bitmap.bits);
		    cur_part->sb->sifs_info.block_bitmap.btmp_bytes_len = 0;
        }

        if(cur_part->sb->sifs_info.inode_bitmap.bits!=NULL){
            // printk("remove inode bitmap\n");
            kfree(cur_part->sb->sifs_info.inode_bitmap.bits);
            cur_part->sb->sifs_info.inode_bitmap.btmp_bytes_len = 0;
        }

        kfree(cur_part->sb);

        close_root_dir(cur_part);
		printk("close root directory\n");
    }

	dlist_traversal(&partition_list,mount_partition,(void*)part_name);
	open_root_dir(cur_part);

	// 同步全局 cur_dir 指向新的 root_dir 内存
    cur_dir_inode = root_dir_inode; 

    // 强制让当前运行进程的路径回到根目录，防止它引用旧分区的 inode 编号
    struct task_struct* cur = get_running_task_struct();
    cur->cwd_inode_nr = cur_part->sb->s_root_ino;
}

// 创建特殊文件（字符/块设备/FIFO）
int32_t sys_mknod(const char* _pathname, enum file_types type, uint32_t dev) {
    if (type != FT_CHAR_SPECIAL && type != FT_BLOCK_SPECIAL && type != FT_FIFO) {
        printk("sys_mknod: only support special files\n");
        return -1;
    }

    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) return -1;

    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); // 统一转成绝对路径

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 查找路径（search_file 内部会 inode_open 父目录）
    int32_t inode_no = search_file(pathname, &searched_record);

    // 检查目标是否已存在
    if (inode_no != -1) {
        printk("sys_mknod: file %s exists! ino:%d\n", pathname, inode_no);
        goto rollback;
    }

    // 检查父目录是否存在
    if (path_depth_cnt((char*)pathname) != path_depth_cnt(searched_record.searched_path)) {
        printk("sys_mknod: parent dir not exist\n");
        goto rollback;
    }

    // 准备元数据
    struct inode* parent_inode = searched_record.parent_inode;
    struct partition* part = get_part_by_rdev(searched_record.i_dev);
    char* filename = strrchr(pathname, '/') + 1;

    // 分配 Inode 编号
    inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        printk("sys_mknod: allocate inode failed\n");
        goto rollback;
    }

    // 初始化特殊的设备 Inode
    struct inode new_inode;
    inode_init(part, inode_no, &new_inode, type);
    // 将设备号存入 inode
    new_inode.i_rdev = dev; 
    new_inode.sifs_i.i_rdev = dev; 
    new_inode.i_size = 0; 

    // 同步新 Inode 和位图到磁盘
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, &new_inode, io_buf);
    bitmap_sync(part, inode_no, INODE_BITMAP);

    // 创建并同步目录项到父目录
    // 使用简单文件系统的 sifs_dir_entry 结构体
    struct sifs_dir_entry new_de;
    memset(&new_de, 0, sizeof(struct sifs_dir_entry));
    sifs_create_dir_entry(filename, inode_no, type, &new_de);

    memset(io_buf, 0, SECTOR_SIZE * 2);
    // 传入 parent_inode 而不是 parent_dir
    if (!sifs_sync_dir_entry(parent_inode, &new_de, io_buf)) {
        printk("sys_mknod: sifs_sync_dir_entry failed\n");
        // 暂时先简单处理一下，回滚位图
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        goto rollback;
    }

    // 同步父目录的 Inode (i_size 已经改变)
    memset(io_buf, 0, SECTOR_SIZE * 2);
    inode_sync(part, parent_inode, io_buf); 

    // 清理并退出
    kfree(io_buf);
    if (parent_inode != root_dir_inode) {
        inode_close(parent_inode);
    }
    return 0;

rollback:
    kfree(io_buf);
    if (searched_record.parent_inode && searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
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
    if (f->fd_inode->i_type == FT_PIPE) {
        struct pipe_inode_info* pii = (struct pipe_inode_info*)&f->fd_inode->pipe_i;
        if (f->fd_flag & O_RDONLY) pii->reader_count++;
        if (f->fd_flag & O_WRONLY) pii->writer_count++;
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
    struct dirent de; // 使用新的通用目录项结构体，这个目录项结构体是同时给用户和 VFS 使用的
    char path[MAX_PATH_LEN];
    fd = sys_open("/dev", O_RDONLY);
    if (fd == -1) {
        // 如果 /dev 不存在，创建它并直接返回
        sys_mkdir("/dev");
        return;
    }

    // 根据重构后的 sys_readdir，它返回 1 表示成功，0 表示结束
    while (sys_readdir(fd, &de) > 0) {

        printk("name:%s\n",de.d_name);
        // 使用 de.d_name 访问文件名
        if (!strcmp(de.d_name, ".") || !strcmp(de.d_name, "..")) {
            continue;
        }

        // 使用 de.d_type 判断文件类型
        if (de.d_type != FT_DIRECTORY) {
            memset(path, 0, MAX_PATH_LEN);
            sprintf(path, "/dev/%s", de.d_name);
            printk("clear_dev: delete file: %s\n", path);
            
            sys_unlink(path);
                
        }
    }

    sys_close(fd);

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

int32_t sys_mkfifo(const char* pathname) {
    // 逻辑基本等同于 sys_mknod(pathname, FT_FIFO, 0)
	// 因为 fifo 和设备inode一样，都是只有inode没有对应磁盘数据块
    return sys_mknod(pathname, FT_FIFO, 0); 
}