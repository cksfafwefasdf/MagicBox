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
#include <file.h>


// root_part 用于记录根分区，他是全局唯一的
struct partition* root_part;
struct inode* cur_dir_inode;

struct dlist file_systems;

// 由于我们去除了 dir 结构，因此现在改用 inode 来标记根目录
struct inode* root_dir_inode; 

// 注册文件系统
static void register_filesystem(struct file_system_type *fs) {
    // 简单地推入后台即可
    dlist_push_back(&file_systems, &fs->fs_elem);
}

// 辅助函数，用于遍历查找匹配的文件系统名
static bool compare_fs_name(struct dlist_elem* elem, void* arg) {
    char* name = (char*)arg;
    struct file_system_type* fs = member_to_entry(struct file_system_type, fs_elem, elem);
    return (strcmp(fs->name, name) == 0);
}

// 查找函数，供 sys_mount 调用 
static struct file_system_type *find_filesystem(const char *name) {
    struct dlist_elem* found = dlist_traversal(&file_systems, compare_fs_name, (void*)name);
    if (found) {
        return member_to_entry(struct file_system_type, fs_elem, found);
    }
    return NULL;
}

static bool mount_root_partition(struct dlist_elem* pelem, void* arg) {
    char* part_name = (char*)arg;
    struct partition* part = member_to_entry(struct partition, part_tag, pelem);
    
    if (strcmp(part->name, part_name) != 0) return false;

    // 申请 VFS 级别的超级块内存
    struct super_block* sb = kmalloc(sizeof(struct super_block));
    if (!sb) PANIC("VFS: kmalloc super_block failed!");
    memset(sb, 0, sizeof(struct super_block));

    // 设置通用字段
    sb->s_dev = part->i_rdev;

    // 遍历所有已注册的文件系统类型进行探测
    struct dlist_elem* fs_elem = file_systems.head.next;
    bool mounted = false;

    while (fs_elem != &file_systems.tail) {
        struct file_system_type* fst = member_to_entry(struct file_system_type, fs_elem, fs_elem);
        
        // 只尝试需要设备的物理文件系统
        if (fst->flags & FS_REQUIRES_DEV) {
            // 尝试调用该文件系统的读取函数
            // silent 参数设为 1，因为探测失败是正常的，不需要打印报错
            if (fst->read_super(sb, NULL, 1) != NULL) {
                mounted = true;
                printk("VFS: mounted root partition %s as %s\n", part_name, fst->name);
                break; 
            }
        }
        fs_elem = fs_elem->next;
    }

    if (mounted) {
        root_part = part;
        return true;
    } else {
        kfree(sb);
        printk("VFS: failed to mount root: no filesystem recognized on %s\n", part_name);
        return false;
    }
}

// 检查是否具有文件系统，如果是的话返回对应魔数，否则返回-1
static int32_t has_fs(struct partition* part){
    // 临时申请一个磁盘超级块缓冲区，仅用于探测
    void* sb_buf = kmalloc(SECTOR_SIZE);
    if (sb_buf == NULL) PANIC("has_fs: kmalloc failed!");
    partition_read(part,1, sb_buf, 1);
    int32_t ret = -1;
    // 检查魔数，如果没有则格式化
    
    if (((struct sifs_super_block*)sb_buf)->magic == SIFS_FS_MAGIC_NUMBER) {
        ret = SIFS_FS_MAGIC_NUMBER;
    }
    // else if(((struct ext2_super_block*)sb_buf)->magic != EXT2_FS_MAGIC_NUMBER)
    
    // 探测完毕，释放临时缓冲区
    kfree(sb_buf);
    return ret;
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

                    if(has_fs(part)==-1){
                        printk("Formatting %s's partition %s ......\n", hd->name, part->name);
                        sifs_format(part);
                    }
                    // 记录第一个有效分区的名字，准备挂载
                    if (first_flag) {
                        strcpy(default_part, part->name);
                        first_flag = false;
                    }
                    // 我们只默认格式化识别到的第一个分区
                    goto mnt;
                }
                part_idx++;
                part++;
            }
            dev_no++;
        }
        channel_no++;
    }

mnt:
    dlist_init(&file_systems);

    register_filesystem(&sifs_fs_type);

    // 调用 mount_partition 进行挂载
    // mount_partition 内部会调用 sifs_read_super，加载超级块
    // 而 sifs_read_super 会完成分配 VFS super_block、位图、根 inode 的所有工作
    if (default_part[0] != 0) {
        dlist_traversal(&partition_list, mount_root_partition, (void*)default_part);
    } else {
        PANIC("No available partition to mount!");
    }

    // 设置当前目录环境
    // sifs_read_super 已经把 root_inode 存进了 root_part->sb->s_root_inode
    cur_dir_inode = root_part->sb->s_root_inode; 
    open_root_dir(root_part);
    // 初始化全局文件表
    uint32_t fd_idx = 0;
    while (fd_idx < MAX_FILE_OPEN_IN_SYSTEM) {
        file_table[fd_idx++].fd_inode = NULL;
    }
    printk("filesys_init done!\n");
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
static int32_t search_file(char* pathname, struct path_search_record* searched_record) {
    // 处理根目录特例
    if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/..")) {
        searched_record->parent_inode = inode_open(root_part, root_dir_inode->i_no); 
        searched_record->file_type = FT_DIRECTORY;
        searched_record->searched_path[0] = 0;
        searched_record->i_dev = root_dir_inode->i_dev;
        return root_dir_inode->i_no;
    }

    ASSERT(pathname[0] == '/' && strlen(pathname) < MAX_PATH_LEN);

    // 初始起点，根目录
    struct inode* curr_inode = inode_open(root_part, root_dir_inode->i_no);
    char* p = pathname;
    
    searched_record->parent_inode = curr_inode;
    searched_record->file_type = FT_UNKNOWN;

    // 开始迭代解析路径组件
    while (*p) {
        // 跳过重复的 '/'
        while (*p == '/') p++;
        if (!*p) break; // 路径以 / 结尾，如 "/dev/"

        // 计算当前组件的长度，例如 "/usr/bin" 中的 "usr"
        char* name_start = p;
        while (*p && *p != '/') p++;
        int len = p - name_start;

        // 记录已扫描路径 (用于调试或记录)
        strcat(searched_record->searched_path, "/");
        strncat(searched_record->searched_path, name_start, len);

        // 处理 .. 向上穿透 (挂载点)
        if (len == 2 && memcmp(name_start, "..", 2) == 0) {
            if (curr_inode->i_no == curr_inode->i_sb->s_root_ino && curr_inode->i_mount_at) {
                struct inode* old = curr_inode;
                struct inode* mp = curr_inode->i_mount_at;
                curr_inode = inode_open(get_part_by_rdev(mp->i_dev), mp->i_no);
                if (old != root_dir_inode) inode_close(old);
            }
        }

        // 调用分层后的 lookup
        struct inode* next_inode = NULL;
        int ret = -1;
        // 暂时先硬编码，等inode操作全部抽离结束后更改
        if(curr_inode->i_op!=NULL&&curr_inode->i_op->lookup!=NULL){
            ret = curr_inode->i_op->lookup(curr_inode, name_start, len, &next_inode);
        }else{
            PANIC("i_op is NULL!");
        }
        

        if (ret != 0) {
            // 没找到组件，返回 -1。此时 searched_record->parent_inode 仍指向上一级
            searched_record->parent_inode = curr_inode;
            searched_record->i_dev = curr_inode->i_dev;
            return -1; 
        }

        // 处理向下跳转 (挂载点) 
        while (next_inode->i_mount != NULL) {
            struct inode* mounted_root = next_inode->i_mount;
            inode_open(get_part_by_rdev(mounted_root->i_dev), mounted_root->i_no);
            inode_close(next_inode);
            next_inode = mounted_root;
        }

        // 推进迭代 
        if (*p == 0) { 
            // 已经是路径最后一段了
            searched_record->file_type = next_inode->i_type;
            uint32_t ino = next_inode->i_no;
            searched_record->parent_inode = curr_inode; // 保持父目录打开
            searched_record->i_dev = next_inode->i_dev;
            inode_close(next_inode); 
            return ino;
        } else {
            // 还没走完，next_inode 成为下一轮的 curr_inode (父目录)
            if (curr_inode != root_dir_inode) {
                inode_close(curr_inode);
            }
            curr_inode = next_inode;
            searched_record->parent_inode = curr_inode;
            searched_record->i_dev = curr_inode->i_dev;
        }
    }

    // 处理 "/usr/" 这种以斜杠结尾的情况
    searched_record->file_type = FT_DIRECTORY;
    searched_record->i_dev = curr_inode->i_dev;
    return curr_inode->i_no;
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

    int32_t final_inode_no = -1;

	if (flags&O_CREATE) {
        printk("creating file\n");
        char* filename = strrchr(pathname, '/') + 1;
        
        // sifs_create 只会负责持久化文件，他不会建立运行时数据
        // 比如建立全局打开表项或者建立inode缓存等
        // 这个操作应该是要放到 VFS 层做的而不是具体的文件系统来做
        // 因为不管是什么文件系统下的文件，他们file结构的填充和inode缓存的建立都是一样的
        if(searched_record.parent_inode->i_op!=NULL&&searched_record.parent_inode->i_op->create!=NULL){
            final_inode_no = searched_record.parent_inode->i_op->create(searched_record.parent_inode, filename, strlen(filename), 0);
        }else{
            PANIC("searched_record.parent_inode->i_op is NULL");
        }
        
        // sifs_create 返回结果小于0时是错误码，大于0时是 inode 编号
        // 一般不会是0，因为这被stdin和stdout占了
        if (final_inode_no < 0) {
            inode_close(searched_record.parent_inode);
            return -1;
        } 

    } else {
        // file is existed, open the file
		// O_RDONLY,O_WRONLY,O_RDWR
        final_inode_no = inode_no;
    }

    struct partition* part = get_part_by_rdev(searched_record.i_dev);
    fd = file_open(part,final_inode_no,flags);

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

    inode_close(searched_record.parent_inode);
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
            return sifs_file_write(wr_file, buf, count);

        default:
            printk("sys_write: unknown file type %x!\n", type);
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
            return sifs_file_read(rd_file, buf, count);

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

    // inode_close 是文件系统无关的，可以调
    if (file_idx < MAX_FILE_OPEN_IN_SYSTEM) {
        if (searched_record.parent_inode != root_dir_inode) {
            inode_close(searched_record.parent_inode);
        }
        printk("file %s is in use, not allowed to delete!\n", pathname);
        return -1;
    }

    // 执行磁盘数据删除逻辑

    // 从路径中提取最后一个文件名组件 (例如 /a/b/c -> "c")
    char* last_name = strrchr(pathname, '/') + 1;
    uint32_t name_len = strlen(last_name);

    struct inode* parent_inode = searched_record.parent_inode;

    // 先硬编码文件系统类型，等下来改
    if(parent_inode->i_op!=NULL&&parent_inode->i_op->unlink!=NULL){
        parent_inode->i_op->unlink(parent_inode, last_name, name_len);
    }else{
        PANIC("parent_inode->i_op->unlink is NULL");
    }
  
    // 清理 search_file 留在内存里的父目录句柄
    if (searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }
    
    return 0;
}

// mkdir 和 rmdir 是放到 i_op 中的成员，他就是和文件系统强相关的，所以直接写sifs没问题
// 直接操作 sifs 的超级块信息也没啥问题
int32_t sys_mkdir(const char* _pathname) {
    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); 

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 搜索路径
    int32_t inode_no = search_file(pathname, &searched_record);

    // 校验目录是否已存在
    if (inode_no != -1) { 
        printk("sys_mkdir: file or directory %s already exists!\n", pathname);
        goto clean;
    } 

    // 校验父目录是否存在
    uint32_t pathname_depth = path_depth_cnt((char*)pathname);
    uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);
    if (pathname_depth != path_searched_depth) {
        printk("sys_mkdir: subpath %s does not exist\n", searched_record.searched_path);
        goto clean;
    }


    struct inode* parent_inode = searched_record.parent_inode;
    // 获取待创建的目录名（最后一级路径）
    char* dir_name = strrchr(pathname, '/') + 1;
    uint32_t len = strlen(dir_name);

    int32_t dir_i_no = -1;
    if(parent_inode->i_op != NULL && parent_inode->i_op->mkdir!=NULL){
        dir_i_no = parent_inode->i_op->mkdir(parent_inode, dir_name, len, 0);
    }else{
        PANIC("parent_inode->i_op is NULL");
    }
    
clean:
    if (searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }

    return (dir_i_no >= 0) ? 0 : dir_i_no;
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
    if (inode_no == 0) {
        printk("sys_rmdir: root directory cannot be removed!\n");
        goto clean;
    }
    
    if (inode_no == -1) {
        printk("In %s, sub path %s not exist\n", pathname, searched_record.searched_path);
        // 即便没找到，如果 parent 不是根目录，也要释放 search_file 产生的引用
        goto clean;
    }

    // 检查类型
    if (searched_record.file_type != FT_DIRECTORY) {
        printk("sys_rmdir: %s is not a directory\n", pathname);
        goto clean;
    }

    // 调用 FS 层执行磁盘操作
    char* dir_name = strrchr(pathname, '/') + 1;
    uint32_t len = strlen(dir_name);
    int32_t ret = -1;
    if(searched_record.parent_inode->i_op!=NULL&&searched_record.parent_inode->i_op->rmdir!=NULL){
        ret = searched_record.parent_inode->i_op->rmdir(searched_record.parent_inode, dir_name, len);
    }else{
        PANIC("searched_record.parent_inode->i_op is NULL");
    }

clean:
    if (searched_record.parent_inode && searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }
    // ret的其他错误码需要在此处处理，目前先不处理了，直接返回
    // 后期再补
    return (ret == 0) ? 0 : -1;
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
    partition_read(part, block_lba, io_buf, 1);
    
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
        partition_read(part, parent_dir_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
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
            partition_read(part, all_blocks_addr[block_idx], io_buf, 1);
            
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

char* sys_getcwd(char* buf, uint32_t size) {
    ASSERT(buf != NULL);
    void* io_buf = kmalloc(SECTOR_SIZE);
    if (io_buf == NULL) return NULL;

    struct task_struct* cur_thread = get_running_task_struct();
    memset(buf, 0, size);
    char full_path_reverse[MAX_PATH_LEN] = {0};

    // 使用临时指针进行回溯，不要直接修改 cur_thread->pwd
    // 增加引用计数防止回溯过程中被释放
    struct inode* cursor = inode_open(get_part_by_rdev(cur_thread->pwd->i_dev), cur_thread->pwd->i_no);


    // 只要不是“真正的全局根目录”就继续向上回溯。
     // 真正的全局根 Inode 编号为 0 且 没有向上挂载的指针(i_mount_at == NULL)
    while (!(cursor->i_no == 0 && cursor->i_mount_at == NULL)) {
        struct partition* cur_part = get_part_by_rdev(cursor->i_dev);
        uint32_t p_inode_nr;
        uint32_t c_inode_nr = cursor->i_no;

        if (c_inode_nr == 0 && cursor->i_mount_at != NULL) {
            // 情况 1，遇到了子分区的根。
            // 我们需要穿透隧道，回到父分区的挂载点目录。
            struct inode* mp_inode = cursor->i_mount_at; // 拿到挂载点实体(在父分区中)
            struct partition* p_part = get_part_by_rdev(mp_inode->i_dev);
            
            // 找到挂载点在父分区中的父目录编号
            p_inode_nr = get_parent_dir_inode_nr(p_part, mp_inode->i_no, io_buf);
            // 获取挂载点的名字 (例如 "sdb1")
            get_child_dir_name(p_part, p_inode_nr, mp_inode->i_no, full_path_reverse, io_buf);
            
            // 切换 cursor 到父分区的父目录，准备下一轮循环
            inode_close(cursor);
            cursor = inode_open(p_part, p_inode_nr);
        } 
        else {
            // 情况 2，普通目录回溯。
            p_inode_nr = get_parent_dir_inode_nr(cur_part, c_inode_nr, io_buf);
            if (get_child_dir_name(cur_part, p_inode_nr, c_inode_nr, full_path_reverse, io_buf) == -1) {
                // 如果回溯失败（理论不应发生）
                inode_close(cursor);
                kfree(io_buf);
                return NULL;
            }
            
            // 切换到父目录
            inode_close(cursor);
            cursor = inode_open(cur_part, p_inode_nr);
        }
    }

    // 释放最后停在全局根上的 cursor
    inode_close(cursor);

    // 处理特殊情况：如果路径为空，说明当前就在全局根 "/"
    if (full_path_reverse[0] == 0) {
        buf[0] = '/';
        buf[1] = 0;
    } else {
        // 路径反转拷贝逻辑。
        // full_path_reverse 里的格式是 "/name2/name1" (从下往上拼的)
        // 我们需要通过 strrchr 逐个切下来拼成正确的顺序
        char* last_slash;
        while ((last_slash = strrchr(full_path_reverse, '/'))) {
            strcat(buf, last_slash);
            *last_slash = 0; // 截断处理下一段
        }
    }

    kfree(io_buf);
    return buf;
}

int32_t sys_chdir(const char* _pathname) {
    struct path_search_record record;
    memset(&record, 0, sizeof(record));

    char path[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, path); 

    int inode_no = search_file(path, &record);

    if (inode_no == -1) {
        printk("sys_chdir: %s not exist\n", path);
        return -1;
    }

    if (record.file_type != FT_DIRECTORY) {
        printk("sys_chdir: %s is not a directory\n", path);
        inode_close(record.parent_inode);
        return -1;
    }

    // 目录切换
    struct task_struct* cur = get_running_task_struct();
    
    // 打开目标目录的 Inode (增加引用计数)
    // 如果是穿透挂载点，search_file 返回的是子分区根编号
    struct inode* new_pwd = inode_open(get_part_by_rdev(record.i_dev), inode_no);

    // printk("CHDIR_FINAL: Going to Dev %x, Ino %d\n", record.i_dev, inode_no);

    // 释放旧的当前目录 (减少引用计数)
    if (cur->pwd != NULL) {
        inode_close(cur->pwd);
    }

    // 这里的 parent_inode 是 search_file 产生的中间产物，必须关闭
    inode_close(record.parent_inode);

    // 更新进程的工作目录指针
    cur->pwd = new_pwd;

    return 0;
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

    // 磁盘容量单位处理逻
    char** granularits = kmalloc(sizeof(char*) * disk_num);
    uint8_t* div_cnts = kmalloc(sizeof(uint8_t) * disk_num);
    memset(div_cnts, 0, disk_num);

    for (int i = 0; i < disk_num; i++) {
        uint32_t temp_size = disk_size[i];
        while (temp_size > 1024) {
            temp_size /= 1024;
            div_cnts[i]++;
        }
        char* units[] = {"B", "KB", "MB", "GB", "TB"};
        granularits[i] = (div_cnts[i] < 5) ? units[div_cnts[i]] : "OVERFLOW!";
    }

    // 打印磁盘基本容量 (sda, sdb...)
    for (channel_idx = 0; channel_idx < CHANNEL_NUM; channel_idx++) {
        for (uint8_t ide_idx = 0; ide_idx < DEVICE_NUM_PER_CHANNEL; ide_idx++) {
            struct disk* hd = &channels[channel_idx].devices[ide_idx];
            if (!hd->name[0]) continue;
            uint8_t d_idx = channel_idx * DISK_NUM_IN_CHANNEL + ide_idx;
            uint32_t display_size = disk_size[d_idx];
            for(int j=0; j<div_cnts[d_idx]; j++) display_size /= 1024;
            printk("%s\t%d%s\n", hd->name, display_size, granularits[d_idx]);
        }
    }

    printk("partition\tformatlize\t512B-blocks\tused\tavaliable\n");

    // 分区遍历与统计
    for (channel_idx = 0; channel_idx < CHANNEL_NUM; channel_idx++) {
        for (uint8_t device_idx = 0; device_idx < DEVICE_NUM_PER_CHANNEL; device_idx++) {
            struct disk* hd = &channels[channel_idx].devices[device_idx];
            if (!hd->name[0]) continue;

            bool has_partition = false;
            for (uint8_t i = 0; i < PRIM_PARTS_NUM; i++) {
                if (hd->prim_parts[i].name[0]) { has_partition = true; break; }
            }

            if (has_partition) {
                for (int type = 0; type < 2; type++) {
                    int limit = (type == 0) ? PRIM_PARTS_NUM : LOGIC_PARTS_NUM;
                    for (int p_idx = 0; p_idx < limit; p_idx++) {
                        struct partition* part = (type == 0) ? &hd->prim_parts[p_idx] : &hd->logic_parts[p_idx];
                        if (!part->name[0]) continue;

                        struct statfs st = {0};
                        bool is_sifs = false;
                        
                        // 如果挂载了，走 VFS 接口；没挂载，走临时读取逻辑
                        /*
                            硬编码稍后要替换成操作集
                            if (part->sb != NULL && part->sb->s_op->statfs != NULL) {
                                part->sb->s_op->statfs(part->sb, &st);
                            } 
                        */
                        if (part->sb != NULL && part->sb->s_op != NULL) {
                            part->sb->s_op->statfs(part->sb, &st);
                            is_sifs = true;
                        } else {
                            struct sifs_super_block temp_sb;
                            partition_read(part, 1, &temp_sb, 1);
                            if (temp_sb.magic == SIFS_FS_MAGIC_NUMBER) {
                                is_sifs = true;
                                st.f_type = temp_sb.magic;
                                st.f_blocks = temp_sb.sec_cnt;

                                // 由于没挂载，因此这里需要现场扫位图
                                uint32_t btmp_sects = temp_sb.block_bitmap_sects;
                                uint8_t* btmp_buf = kmalloc(btmp_sects * SECTOR_SIZE);
                                if (btmp_buf) {
                                    partition_read(part, temp_sb.block_bitmap_lba, btmp_buf, btmp_sects);
                                    struct bitmap btmp = {.bits = btmp_buf, .btmp_bytes_len = temp_sb.sec_cnt / 8};
                                    st.f_bfree = bitmap_count(&btmp);
                                    kfree(btmp_buf);
                                }
                            }
                        }

                        if (!is_sifs) {
                            // 没有文件系统就输出 -
                            printk("%s(%c)\t-\t-\t-\t-\n", part->name, (type == 0 ? 'P' : 'L'));
                        } else {
                            // 计算位图并打印
                            uint32_t used_sects = st.f_blocks - st.f_bfree;
                            
                            // 根分区打 * 标记
                            char root_flag_str[2] = {0};
                            if (root_part != NULL && !strcmp(root_part->name, part->name)) {
                                root_flag_str[0] = '*';
                            }

                            // 完全还原你的 printk 格式
                            printk("%s(%c)%s\t%x\t%d\t%d\t%d\n", 
                                part->name, 
                                (type == 0 ? 'P' : 'L'), 
                                root_flag_str, 
                                st.f_type, 
                                st.f_blocks, 
                                used_sects, 
                                st.f_bfree);
                        }
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

static struct partition* get_part_by_name(char* part_name) {
    struct dlist_elem* res = dlist_traversal(&partition_list, check_disk_name, (void*)part_name);
    if (res == NULL) {
        return NULL;
    }
    return member_to_entry(struct partition, part_tag, res);
}

// 专门用于在路径解析过头（钻进子分区）时，退回到挂载点本身
static struct inode* back_to_mountpoint(struct inode* inode) {
    if (inode->i_mount == NULL && 
        inode->i_no == inode->i_sb->s_root_ino && 
        inode->i_mount_at != NULL) {
        
        struct inode* mp_inode = inode->i_mount_at;
        // 增加挂载点的引用计数
        inode_open(get_part_by_rdev(mp_inode->i_dev), mp_inode->i_no);

        return mp_inode;
    }
    inode_open(get_part_by_rdev(inode->i_dev), inode->i_no);
    return inode;
}

int32_t sys_mount(char* dev_path, char* _mount_path, char* type, unsigned long new_flags UNUSED, void * data UNUSED) {
    printk("start mounting...\n");
    // 取出设备名
    char* dev_name = strrchr(dev_path,'/') + 1;
    // 通用 VFS 准备工作
    // 获取设备分区
    struct partition* part = get_part_by_name(dev_name);
    if (!part){
        printk("can't find block dev %s\n",dev_name);
        return -1;
    }

    // 防止一个分区被挂载在多个目录下（现在暂时只支持单分区挂载）
    // 这个操作可以有效防止根文件系统例如sda1被再次挂载的情况
    // 如果根 Inode 已经指向了一个挂载点，说明它真的被挂载了
    if (part->sb != NULL && part->sb->s_root_inode != NULL && part->sb->s_root_inode->i_mount_at != NULL) {
        printk("VFS: Device %s is already mounted at some directory!\n", dev_path);
        return -1;
    }

    // 找到挂载点目录
    struct path_search_record record;
    memset(&record, 0, sizeof(record));
    char mount_path[MAX_PATH_LEN] = {0};
    make_abs_pathname(_mount_path, mount_path); // 统一转成绝对路径
    // 在这里使用不分区穿透的版本
    // 不会产生什么问题，因为如果该目录挂载了分区，那么我们就是要不穿透才能进行重复挂载的检查
    // 如果没有挂载分区，那么 do_search_file 第三个参数即使为true也不会穿透，因此置为false就行
    int32_t mp_inode_no = search_file(mount_path, &record);

    if (mp_inode_no == -1){
        printk("fail to find mount point\n");
        return -1;
    }

    struct inode* mp_inode = inode_open(get_part_by_rdev(record.i_dev), mp_inode_no);

    struct inode* back_mp_inode = back_to_mountpoint(mp_inode);

    // 把刚 search_file 为了搜索而打开的父目录关掉
    inode_close(record.parent_inode);

    // 如果当前挂载点已经挂载了其他分区，那么拒绝挂载操作
    if (back_mp_inode->i_mount != NULL) {
        printk("VFS: mount failed! %s is already a mount point for another device.\n", mount_path);
        inode_close(mp_inode);
        inode_close(back_mp_inode);
        return -1;
    }

    inode_close(back_mp_inode); 

    // 挂载点必须是目录，且没被挂载
    if (mp_inode->i_mount != NULL) {
        printk("VFS: %s is already a mount point\n", mount_path);
        inode_close(mp_inode);
        return -1;
    }

    if (mp_inode->i_type != FT_DIRECTORY){
        printk("VFS: mount target must be a direcotry\n", mount_path);
        inode_close(mp_inode);
        return -1;
    }

    // 分配并初始化通用的超级块
    struct super_block* sb = kmalloc(sizeof(struct super_block));
    memset(sb, 0, sizeof(struct super_block));
    sb->s_dev = part->i_rdev;
    part->sb = sb;

    // 超级块加载分发驱动 

    struct file_system_type *fst = find_filesystem(type);
    struct super_block* res = NULL;
    if (fst && (fst->flags & FS_REQUIRES_DEV)) {
        // 获取分区并 read_super
        res = fst->read_super(sb, NULL, 0);
        if(res==NULL){
            printk("fail to mount %s, you should mkfs first!\n",dev_name);
            goto rollback;
        }
    }else{
        printk("VFS: Unknown filesystem type %s\n", type);
        kfree(sb);
        return -1;
    }

    // 建立隧道，向上穿透和向下穿透
    if (res != NULL && res->s_root_inode != NULL) {
        // sifs_read_super 已经帮我们把 root_inode 打开并存在 sb->s_root_inode 了
        struct inode* root_inode = sb->s_root_inode;
        
        mp_inode->i_mount = root_inode; // 向下隧道, mp_inode 相当于是 /mnt/sdb1 中的 sdb1 目录
        root_inode->i_mount_at = mp_inode; // 向上隧道，相当于是 sdb1分区的根目录
        
        // 挂载成功后，mp_inode 应该保持 open 状态（不被释放出内存）
        // 这样隧道才稳定
        printk("mount %s as %s done! mount at %s\n",dev_name,type,mount_path);
        return 0;
    }
rollback:
    // 失败回滚
    printk("VFS: Failed to mount %s\n", dev_name);
    part->sb = NULL; // 清除绑定关系
    inode_close(mp_inode); // 释放挂载点引用
    kfree(sb); // 释放超级块内存
    return -1;
}

int32_t sys_umount(const char* _mount_path) {
    // 找到挂载点目录的 inode
    struct path_search_record record;
    memset(&record, 0, sizeof(record));
    
    char mount_path[MAX_PATH_LEN] = {0};
    make_abs_pathname(_mount_path, mount_path); 

    int32_t mp_inode_no = search_file(mount_path, &record);
    if (mp_inode_no == -1) {
        printk("VFS: umount target %s not found\n", mount_path);
        return -1;
    }

    // search_file 可能带我们钻进了子分区
    struct inode* target_inode = inode_open(get_part_by_rdev(record.i_dev), mp_inode_no);
    
    struct inode* mp_inode = target_inode;

    // search_file 可能会带我进入到子分区的根目录中，这个子分区的根目录肯定不是挂载点
    // 挂载点是例如 mnt/sdb1 的 sdb1，而不是sdb1的根
    // 因此如果是这种情况，我们需要回溯回去
    if (mp_inode->i_mount == NULL && mp_inode->i_no == get_part_by_rdev(mp_inode->i_dev)->sb->s_root_ino && mp_inode->i_mount_at != NULL) {
        // 记录子分区的根
        struct inode* child_root = mp_inode;
        // 拿到真正的挂载点（在主分区里那个目录）
        mp_inode = child_root->i_mount_at;
        
        // 增加挂载点的引用，关闭刚才误入的子分区根
        inode_open(get_part_by_rdev(mp_inode->i_dev), mp_inode->i_no);
        inode_close(child_root);
    }

    inode_close(record.parent_inode);

    // 现在再检查，mp_inode 就是主分区里的那个目录了
    if (mp_inode->i_mount == NULL) {
        printk("VFS: %s is not a mount point\n", mount_path);
        inode_close(mp_inode);
        return -1;
    }

    // 获取被挂载分区的根 inode 和超级块
    struct inode* child_root = mp_inode->i_mount;
    struct super_block* sb = child_root->i_sb;
    struct partition* part = get_part_by_rdev(sb->s_dev);

    // 安全性检查, 检查设备是否繁忙
    // child_root 被两个地方引用：sb->s_root_inode 和 mp_inode->i_mount
    // 此外，inode_open 还会增加引用。
    // 如果 i_open_cnt > 1 说明有进程 PWD 在这或文件打开着
    // 如果等于 1，说明只有 sb 持有，可以释放
    if (child_root->i_open_cnts > 1) {
        printk("VFS: Device %x is busy, open_cnt is %x. Can't umount!\n", sb->s_dev, child_root->i_open_cnts);
        inode_close(mp_inode);
        return -1;
    }

    printk("VFS: start unmounting %s...\n", mount_path);

    // 拆除隧道（断开联系）
    mp_inode->i_mount = NULL; // 断开向下隧道
    child_root->i_mount_at = NULL; // 断开向上隧道

    // 释放文件系统特定资源
    // 释放位图等内存缓冲区（这些是在 sifs_read_super 中分配的）
    
    if(sb->s_op != NULL){
        sb->s_op->put_super(sb);
    }else{
        PANIC("unknown s_op!");
    }

    // 关闭子分区的根 inode（彻底释放出内存）
    // 这一步会触发 inode_close 里的同步逻辑，将其写回磁盘
    inode_close(child_root);

    // 销毁超级块
    kfree(sb);
    part->sb = NULL; // 清除分区与超级块的绑定关系

    // 释放挂载点 inode 的引用，对应本函数开头的 inode_open
    // 在这之后，mp_inode 可能依然被 mount 时的 inode_open 锁定在内存中
    // 我们在 mount 中没有执行 inode_close(mp_inode)，主要是为了维持上下通道稳定
    // 所以这里需要执行两次 close 来抵消引用，或者确保 mount 逻辑与 umount 对称。
    inode_close(mp_inode); 
    
    inode_close(mp_inode); 

    printk("VFS: unmount %s done.\n", mount_path);
    return 0;
}

// 创建特殊文件（字符/块设备/FIFO）
int32_t sys_mknod(const char* _pathname, enum file_types type, uint32_t dev) {
    if (type != FT_CHAR_SPECIAL && type != FT_BLOCK_SPECIAL && type != FT_FIFO) {
        printk("sys_mknod: only support special files\n");
        return -1;
    }

    char pathname[MAX_PATH_LEN] = {0};
    make_abs_pathname(_pathname, pathname); // 统一转成绝对路径

    struct path_search_record searched_record;
    memset(&searched_record, 0, sizeof(struct path_search_record));
    
    // 查找路径（search_file 内部会 inode_open 父目录）
    int32_t inode_no = search_file(pathname, &searched_record);

    // 检查目标是否已存在
    if (inode_no != -1) {
        printk("sys_mknod: file %s exists! ino:%d\n", pathname, inode_no);
        goto clean;
    }

    // 检查父目录是否存在
    if (path_depth_cnt((char*)pathname) != path_depth_cnt(searched_record.searched_path)) {
        printk("sys_mknod: parent dir not exist\n");
        goto clean;
    }

    // 准备元数据
    struct inode* parent_inode = searched_record.parent_inode;
    char* filename = strrchr(pathname, '/') + 1;
    uint32_t len = strlen(filename);
    int32_t ret = -1;
    if(parent_inode->i_op!=NULL&&parent_inode->i_op->mknod!=NULL){
        ret = parent_inode->i_op->mknod(parent_inode, filename, len, (int)type, (int)dev);
    }else{
        PANIC("parent_inode->i_op is NULL");
    }
    
clean:
    // 释放 search_file 打开的引用
    if (searched_record.parent_inode && searched_record.parent_inode != root_dir_inode) {
        inode_close(searched_record.parent_inode);
    }

    return (ret == 0) ? 0 : -1;

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