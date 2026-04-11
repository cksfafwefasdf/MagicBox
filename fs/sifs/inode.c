#include <sifs_inode.h>
#include <ide.h>
#include <stdint.h>
#include <debug.h>
#include <dlist.h>
#include <string.h>
#include <fs.h>
#include <thread.h>
#include <interrupt.h>
#include <stdio-kernel.h>
#include <sifs_file.h>
#include <ide_buffer.h>
#include <fs_types.h>
#include <sifs_sb.h>
#include <sifs_dir.h>
#include <inode.h>
#include <sifs_sb.h>
#include <errno.h>

/*
    此处的各种inode类型操作主要负责磁盘相关的操作
    因为磁盘操作和文件系统强相关，延迟因此这部分内容需要放到这里
    vfs 相关的 或者其他运行时信息放到 vfs 层做
*/

static void sifs_inode_init(struct partition* part, uint32_t inode_no,struct inode* new_inode,enum file_types ft){
    memset(new_inode,0,sizeof(struct inode));
	new_inode->i_no = inode_no;
	new_inode->i_size = 0;
	new_inode->i_open_cnts = 0;
	new_inode->write_deny = false;
	new_inode->i_type = ft;
	new_inode->i_dev = part->i_rdev;
    new_inode->i_sb = part->sb; // 建立归属超级块，以后读写数据块要用到他
    new_inode->i_mount = NULL; // 默认不是挂载点
    new_inode->i_mount_at = NULL; // 默认不是另一个分区的根

    switch (ft) {
        case FT_REGULAR:
            new_inode->i_op = &sifs_file_inode_operations;
            break;
        case FT_DIRECTORY:
            new_inode->i_op = &sifs_dir_inode_operations;
            break;
        case FT_CHAR_SPECIAL:
            new_inode->i_op = &sifs_char_inode_operations;
            break;
        case FT_BLOCK_SPECIAL:
            new_inode->i_op = &sifs_block_inode_operations;
            break;
        default:
            new_inode->i_op = NULL;
            break;
    }

	uint8_t sec_idx = 0;
	while(sec_idx<BLOCK_PTR_NUMBER){
		new_inode->sifs_i.i_sectors[sec_idx]=0;
		sec_idx++;
	}
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

void sifs_inode_release(struct partition* part,uint32_t inode_no){
	struct inode* inode_to_del = inode_open(part,inode_no);
	ASSERT(inode_to_del->i_no==inode_no);

	// 如果是设备 inode，那么就不进行后续的释放操作，因为设备inode更笨
	if (!has_data_blocks(inode_to_del->i_type)){
		return;
	}

	uint8_t block_idx = 0,block_cnt = DIRECT_INDEX_BLOCK;
	uint32_t block_bitmap_idx;
	uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};

	while(block_idx<DIRECT_INDEX_BLOCK){
		all_blocks_addr[block_idx] = inode_to_del->sifs_i.i_sectors[block_idx];
		block_idx++;
	}
	// the first first-level index block
	int tfflib = DIRECT_INDEX_BLOCK;
	if(inode_to_del->sifs_i.i_sectors[tfflib]!=0){
		partition_read(part,inode_to_del->sifs_i.i_sectors[tfflib],all_blocks_addr+tfflib,1);
		block_cnt = TOTAL_BLOCK_COUNT;

		block_bitmap_idx = inode_to_del->sifs_i.i_sectors[tfflib] - part->sb->sifs_info.sb_raw.data_start_lba;
		ASSERT(block_bitmap_idx>0);
		bitmap_set(&part->sb->sifs_info.block_bitmap,block_bitmap_idx,0);
		bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
	}

	block_idx = 0;
	while(block_idx<block_cnt){
		if(all_blocks_addr[block_idx]!=0){
			block_bitmap_idx = 0;
			block_bitmap_idx = all_blocks_addr[block_idx]-part->sb->sifs_info.sb_raw.data_start_lba;
			ASSERT(block_bitmap_idx>0);
			bitmap_set(&part->sb->sifs_info.block_bitmap,block_bitmap_idx,0);
			bitmap_sync(part,block_bitmap_idx,BLOCK_BITMAP);
		}
		block_idx++;
	}

	bitmap_set(&part->sb->sifs_info.inode_bitmap,inode_no,0);
	bitmap_sync(part,inode_no,INODE_BITMAP);

	void* io_buf = kmalloc(SECTOR_SIZE*2);
	sifs_inode_delete(part,inode_no,io_buf);
	kfree(io_buf);

	inode_close(inode_to_del);
}

// 从 inode 的 offset 处读取 count 字节到 buf
// 专门供 swap_page/惰性加载使用，不依赖 fd_table
int32_t sifs_inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count) {

    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint8_t* buf_dst = (uint8_t*)buf;
    uint32_t size_left = count;

    // 边界检查：不能超过文件实际大小
    if (offset + count > inode->i_size) {
        size_left = inode->i_size - offset;
    }
    if (size_left <= 0) return 0;

    // 准备缓冲区和块地址表

    uint8_t* io_buf = kmalloc(SIFS_BLOCK_SIZE);
    uint32_t* all_blocks_addr = (uint32_t*)kmalloc(TOTAL_BLOCK_COUNT * sizeof(uint32_t));
    if (!io_buf || !all_blocks_addr) {
        PANIC("inode_read_data: kmalloc failed");
    }
    memset(all_blocks_addr, 0, TOTAL_BLOCK_COUNT * sizeof(uint32_t));

    // 填充 block 地址表
    uint32_t block_end_idx = (offset + size_left - 1) / SIFS_BLOCK_SIZE;

    // 填充直接块 (0-11)
    uint32_t idx = 0;
    while (idx < DIRECT_INDEX_BLOCK && idx <= block_end_idx) {
        all_blocks_addr[idx] = inode->sifs_i.i_sectors[idx];
        idx++;
    }

	// the first first-level index block
	uint32_t tfflib = DIRECT_INDEX_BLOCK;

    // 填充一级间接块 (12 及以后)
    if (block_end_idx >= tfflib) {
        ASSERT(inode->sifs_i.i_sectors[tfflib] != 0);
        // 从磁盘读取间接索引表到 all_blocks_addr 的后半部分
        partition_read(part, inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    // 开始搬运数据
    uint32_t bytes_read = 0;
    uint32_t curr_pos = offset;

    while (bytes_read < size_left) {
        uint32_t sec_idx = curr_pos / SIFS_BLOCK_SIZE;
        uint32_t sec_lba = all_blocks_addr[sec_idx];
        
        uint32_t sec_off_bytes = curr_pos % SIFS_BLOCK_SIZE;
        uint32_t sec_left_bytes = SIFS_BLOCK_SIZE - sec_off_bytes;
        uint32_t chunk_size = (size_left - bytes_read < sec_left_bytes) ? 
                               (size_left - bytes_read) : sec_left_bytes;

        ASSERT(sec_lba != 0); // 正常文件（非空洞文件）不应为 0

        // 读取一个物理块
        partition_read(part, sec_lba, io_buf, 1);
        
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

// 给定一个 dir 的 inode，从这个inode对应的文件里面找到名为name的项
static int sifs_lookup(struct inode *dir, char *name, int len, struct inode **res) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    struct sifs_dir_entry de;

    // 这里传入了 name 指针和 len，不需要 name 以 \0 结尾
    if (sifs_search_dir_entry(part, dir, name, len, &de)) {
        
        // 找到编号后，打开（或从缓存获取）inode（inode_open会自动处理）
        // 这里返回的是内存中的 inode 结构体指针
        struct inode* target_inode = inode_open(part, de.i_no);
        
        if (target_inode == NULL) {
            // 理论上磁盘有记录但打不开（如内存不足或磁盘损坏）
            *res = NULL;
            return -EIO; 
        }

        *res = target_inode;
        return 0; // 成功找到
    }

    // 没找到时，按照 Linux 规范返回 -ENOENT
    *res = NULL;
    return -ENOENT; 
}

// 同样只做磁盘落盘相关的操作
// 返回值大于0是是inode号，小于零时是错误码
static int32_t sifs_mkdir(struct inode* dir,char* name, int len, int mode UNUSED) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (!io_buf) return -ENOMEM;

    // 分配 Inode
    int32_t inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        kfree(io_buf);
        return -ENOSPC;
    }

    // 在内存初始化这个新 Inode (局部变量)
    struct inode* new_dir_inode = (struct inode*) kmalloc(sizeof(struct inode));
    sifs_inode_init(part, inode_no, new_dir_inode, FT_DIRECTORY);

    // 为新目录分配第一个数据块 (存储 . 和 ..)
    int32_t block_lba = block_bitmap_alloc(part);
    if (block_lba == -1) {
        // 回滚 inode 位图
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        kfree(io_buf);
        return -ENOSPC;
    }
    new_dir_inode->sifs_i.i_sectors[0] = block_lba;
    new_dir_inode->i_size = 2 * part->sb->sifs_info.sb_raw.dir_entry_size;

    // 初始化数据块内容 (. 和 ..)
    memset(io_buf, 0, SECTOR_SIZE * 2);
    struct sifs_dir_entry* p_de = (struct sifs_dir_entry*)io_buf;
    // "."
    sifs_create_dir_entry(".", 1, inode_no, FT_DIRECTORY, p_de);
    // ".."
    sifs_create_dir_entry("..", 2, dir->i_no, FT_DIRECTORY, p_de + 1);
    
    // 写入磁盘
    partition_write(part, block_lba, io_buf, 1);

    // 在父目录中同步新目录项
    struct sifs_dir_entry new_dir_entry;
    sifs_create_dir_entry(name, len, inode_no, FT_DIRECTORY, &new_dir_entry);

    memset(io_buf, 0, SECTOR_SIZE * 2);
    if (!sifs_sync_dir_entry(dir, &new_dir_entry, io_buf)) {
        // 回滚 block 和 inode 位图
        bitmap_set(&part->sb->sifs_info.block_bitmap, (block_lba - part->sb->sifs_info.sb_raw.data_start_lba), 0);
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        kfree(io_buf);
        return -EIO;
    }

    // 同步元数据
    dir->i_sb->s_op->write_inode(dir); // 父目录 i_size 变了
    new_dir_inode->i_sb->s_op->write_inode(new_dir_inode); // 新目录 inode
    bitmap_sync(part, inode_no, INODE_BITMAP);
    bitmap_sync(part, (block_lba - part->sb->sifs_info.sb_raw.data_start_lba), BLOCK_BITMAP);

    kfree(new_dir_inode);
    kfree(io_buf);
    return inode_no;
}

// sifs_rmdir: 执行磁盘层面的目录删除
// dir: 父目录 inode
// name: 待删除目录名
// len: 名字长度
static int32_t sifs_rmdir(struct inode* dir, char* name, int len) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    int32_t retval = -EIO;
    
    // 找到该目录在父目录中的 inode 编号
    struct sifs_dir_entry de;
    if (!sifs_search_dir_entry(part, dir, name, len, &de)) {
        return -ENOENT;
    }

    // 检查该目录是否为空（只有 . 和 ..）
    struct inode* target_inode = inode_open(part, de.i_no);
    if (!sifs_dir_is_empty(target_inode)) {
        inode_close(target_inode);
        return -ENOTEMPTY;
    }

    // 开始执行删除事务
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) {
        inode_close(target_inode);
        return -ENOMEM;
    }

    // 从父目录抹除目录项
    if (sifs_delete_dir_entry(part, dir, de.i_no, io_buf)) {
        // 彻底释放该 Inode 及其占用的磁盘块
        // sifs_inode_release 内部会处理位图同步
        sifs_inode_release(part, de.i_no);
        
        // 为了保证内存一致性，强制从缓存驱逐该 Inode，确保它不会再被访问
        // 我们刚刚在上面 inode_open 了它，现在要 evict 它
        // 上面那个open是由于我们需要检查目录是否是一个空目录在这么做的
        // inode_evict(target_inode); 
        retval = 0;
    } else {
        inode_close(target_inode);
    }
    inode_evict(target_inode);

    kfree(io_buf);
    return retval;
}

// sifs_mknod: 磁盘层面的特殊文件创建
// dir: 父目录 inode
// name: 文件名
// len: 名字长度
// type: 文件类型（字符设备、块设备、FIFO）
// dev: 设备号
static int32_t sifs_mknod(struct inode* dir, char* name, int len, int type, int dev) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    
    // 分配 Inode 编号
    int32_t inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) return -ENOSPC;

    // 初始化 Inode
    // 这里需要将 int 类型的 type 显式转为 enum file_types
    // 由于我们没有权限相关的操作，因此直接强转就行
    struct inode new_inode;
    
    sifs_inode_init(part, inode_no, &new_inode, (enum file_types)type);
    new_inode.i_rdev = dev; 
    new_inode.sifs_i.i_rdev = dev; 
    new_inode.i_size = 0; 

    // 同步目录项
    struct sifs_dir_entry new_de;
    sifs_create_dir_entry(name, len, inode_no, (enum file_types)type, &new_de);

    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (!io_buf) {
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        return -ENOMEM;
    }

    if (!sifs_sync_dir_entry(dir, &new_de, io_buf)) {
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        kfree(io_buf);
        return -EIO;
    }

    // 同步元数据
    dir->i_sb->s_op->write_inode(dir);
    new_inode.i_sb->s_op->write_inode(&new_inode);
    bitmap_sync(part, inode_no, INODE_BITMAP);

    kfree(io_buf);
    return 0;
}

// 和 create 一样，只负责文件系统相关的磁盘操作，不负责运行时状态的维护
static int32_t sifs_unlink(struct inode* dir, char* name, int len) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (!io_buf) return -1;

    // 在父目录中查找该名字对应的 inode_no (复用 lookup)
    struct inode* target_inode = NULL;
    int ret = sifs_lookup(dir, name, len, &target_inode);
    if (ret != 0) {
        kfree(io_buf);
        return -1; // 文件不存在
    }

    // 检查类型，unlink 只能删文件，不能删目录
    // 其实我们之前在sys_unlink里已经拦截过一遍了，这里不妨再拦一遍
    // 这个逻辑一般情况下不会被执行
    if (target_inode->i_type == FT_DIRECTORY) {
        inode_close(target_inode);
        kfree(io_buf);
        return -1; // 应该调用 rmdir
    }

    uint32_t inode_no = target_inode->i_no;

    // 执行磁盘抹除逻辑
    // 从父目录删除条目
    if (sifs_delete_dir_entry(part, dir, inode_no, io_buf)) {
        // 释放数据块和 Inode 节点
        sifs_inode_release(part, inode_no);
    }

    // 清理
    inode_close(target_inode); // 释放刚才 lookup 打开的引用
    // 强制清空缓存
    inode_evict(target_inode);

    kfree(io_buf);
    return 0;
}

// mode 是权限字段，由于我们暂时不考虑多用户，因此该字段忽略
// len 是文件名的长度，它通常用于从路径名 name 中来截取相应的文件名
// 返回结果是一个fd
// 该函数只负责磁盘部分的操作，其他的运行时相关的操作交给vfs层
// sifs_create 返回结果小于0时是错误码，大于0时是 inode 编号
static int32_t sifs_create(struct inode *dir, char *name, int len, int mode UNUSED) {
    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (!io_buf) return -ENOMEM;

    struct partition* part = get_part_by_rdev(dir->i_dev);
    
    // 分配 Inode 编号
    int32_t inode_no = inode_bitmap_alloc(part);
    if (inode_no == -1) {
        kfree(io_buf);
        return -ENOSPC; // 磁盘空间不足（Inode 用完了）
    }

    // 初始化内存 Inode
    struct inode* new_file_inode = (struct inode*)kmalloc(sizeof(struct inode));
    if (!new_file_inode) {
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0); // 回滚位图
        kfree(io_buf);
        return -ENOMEM;
    }
    // 初始化一个inode
    sifs_inode_init(part, inode_no, new_file_inode, FT_REGULAR);
    
    // 在父目录中创建目录项
    struct sifs_dir_entry new_dir_entry;
    memset(&new_dir_entry, 0, sizeof(struct sifs_dir_entry));
    
    sifs_create_dir_entry(name, len, inode_no, FT_REGULAR, &new_dir_entry);

    if (!sifs_sync_dir_entry(dir, &new_dir_entry, io_buf)) {
        // 回滚逻辑 (释放 inode_no, 释放 new_file_inode)
        kfree(new_file_inode);
        bitmap_set(&part->sb->sifs_info.inode_bitmap, inode_no, 0);
        kfree(io_buf);
        return -EIO;
    }

    // 持久化元数据 
    dir->i_sb->s_op->write_inode(dir); // 更新父目录 size
    new_file_inode->i_sb->s_op->write_inode(new_file_inode); // 更新新文件
    bitmap_sync(part, inode_no, INODE_BITMAP); // 同步位图
    
    kfree(new_file_inode);
    kfree(io_buf);
    return inode_no; // 成功
}

// 普通文件的 Inode 操作集
struct inode_operations sifs_file_inode_operations = {
    .default_file_ops = &sifs_file_file_operations,
    .create     = NULL,      // 普通文件下通常不能再创建文件
    .lookup     = NULL,      // 普通文件没有子项
    .unlink     = NULL,
    .mkdir      = NULL,
    .rmdir      = NULL,
    .mknod      = NULL,
    .rename     = NULL,
    .bmap       = NULL,      // 后期实现
    .truncate   = NULL,
    .symlink    = NULL, 
    .readlink   = NULL,
    .link       = NULL,
};

// 目录文件的 Inode 操作集
struct inode_operations sifs_dir_inode_operations = {
    .default_file_ops = &sifs_dir_file_operations,
    .create     = sifs_create,
    .lookup     = sifs_lookup,
    .unlink     = sifs_unlink,
    .mkdir      = sifs_mkdir,
    .rmdir      = sifs_rmdir,
    .mknod      = sifs_mknod,
    .rename     = NULL,      // 暂时忽略，等file写完后再补
    .bmap       = NULL,      // 目录通常不需要 bmap
    .truncate   = NULL,
    .symlink    = NULL, 
    .readlink   = NULL,
    .link       = NULL,
};

// 设备文件基本上也没什么inode操作，主要是用来指向相应的default_file_ops操作
// 字符设备的 Inode 操作集
struct inode_operations sifs_char_inode_operations = {
    // .default_file_ops = &sifs_char_dev_operations, // 指向设备通用的文件操作，后期来补
    .default_file_ops = NULL,
    .create     = NULL,      // 普通文件下通常不能再创建文件
    .lookup     = NULL,      // 普通文件没有子项
    .unlink     = NULL,
    .mkdir      = NULL,
    .rmdir      = NULL,
    .mknod      = NULL,
    .rename     = NULL,
    .bmap       = NULL, 
    .truncate   = NULL,
    .symlink    = NULL, 
    .readlink   = NULL,
    .link       = NULL,
};

// 块设备的 Inode 操作集
struct inode_operations sifs_block_inode_operations = {
    // .default_file_ops = &ide_file_operations, // 指向块设备通用的文件操作，后期来补
    .default_file_ops = NULL, // 指向块设备通用的文件操作，后期来补
    .create     = NULL, 
    .lookup     = NULL,
    .unlink     = NULL,
    .mkdir      = NULL,
    .rmdir      = NULL,
    .mknod      = NULL,
    .rename     = NULL,
    .bmap       = NULL, 
    .truncate   = NULL,
    .symlink    = NULL, 
    .readlink   = NULL,
    .link       = NULL,
};