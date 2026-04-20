#include <ext2_inode.h>
#include <ext2_fs.h>
#include <ext2_file.h>
#include <fs_types.h>
#include <errno.h>
#include <stdio-kernel.h>
#include <vgacon.h>
#include <ide.h>
#include <inode.h>
#include <debug.h>
#include <fs_types.h>
#include <unitype.h>
#include <time.h>
#include <inode.h>

static int32_t ext2_bmap(struct inode* inode, int32_t _index);

// 这是一个适配 Ext2 的 Inode 分配函数
// 由于块组的存在，我们无法直接复用sifs的位图操作逻辑，因为sifs只有一个inode位图和块位图
// 而ext2中每一个块位图或者inode位图都是被分散到块组中的，我们无法像sifs那样一次性全部读取，只能用谁读谁
// Ext2 资源分配通用函数 (Inode 或 Block 位图)
// 成功返回全局编号/物理块号，失败返回 -1
// 当申请资源时，先在父目录所在的块组里面申请，如果没有空间，那么就往后面的块组申请
// 如果一直到最后都没有，那么就环回，直到重新碰上自己
int32_t ext2_resource_alloc(struct super_block *sb, uint32_t start_group, enum ext2_bitmap_type type) {

    struct ext2_sb_info *ext2_info = &sb->ext2_info;
    struct partition *part = get_part_by_rdev(sb->s_dev);
    // 组描述符的数量和组数是对应的
    uint32_t total_groups = ext2_info->group_desc_cnt;
    
    uint8_t* io_buf = kmalloc(sb->s_block_size);
    if (!io_buf) return -ENOMEM;

    // 循环遍历所有块组
    for (uint32_t i = 0; i < total_groups; i++) {
        // 实现从 start_group 开始的绕回查找
        uint32_t group = (start_group + i) % total_groups;
        
        uint32_t btmp_block;
        uint32_t bits_per_group;
        uint32_t free_count;

        // 获取该组的元数据和剩余量
        if (type == EXT2_INODE_BITMAP) {
            btmp_block = ext2_info->group_desc[group].bg_inode_bitmap;
            bits_per_group = ext2_info->sb_raw.s_inodes_per_group;
            free_count = ext2_info->group_desc[group].bg_free_inodes_count;
        } else {
            btmp_block = ext2_info->group_desc[group].bg_block_bitmap;
            bits_per_group = ext2_info->sb_raw.s_blocks_per_group;
            free_count = ext2_info->group_desc[group].bg_free_blocks_count;
        }

        // 快速判断, 如果组描述符说这组已经没空位了，直接跳过，节省 IO
        if (free_count == 0) continue;

        // 读取 Bitmap 并进行实际扫描
        partition_read(part, BLOCK_TO_SECTOR(sb, btmp_block), io_buf, sb->s_block_size / SECTOR_SIZE);
        struct bitmap btmp = { .btmp_bytes_len = bits_per_group / 8, .bits = io_buf };
        int bit_idx = bitmap_scan(&btmp, 1);
        
        if (bit_idx != -1) {
            // 找到了，执行标记、写回并返回
            bitmap_set(&btmp, bit_idx, 1);
            partition_write(part, BLOCK_TO_SECTOR(sb, btmp_block), io_buf, sb->s_block_size / SECTOR_SIZE);
            kfree(io_buf);

            // 返回全局编号
            if (type == EXT2_INODE_BITMAP) {
                ext2_info->group_desc[group].bg_free_inodes_count--;
                ext2_info->sb_raw.s_free_inodes_count--;
                // 分配全局唯一的inode号
                return (int32_t)(group * bits_per_group + bit_idx + 1);
            } else {
                ext2_info->group_desc[group].bg_free_blocks_count--;
                ext2_info->sb_raw.s_free_blocks_count--;
                return (int32_t)(ext2_info->sb_raw.s_first_data_block + group * bits_per_group + bit_idx);
            }
        }
        
        // 如果 bit_idx 为 -1，说明描述符可能和实际位图不一致（虽然不该发生），继续下一组
    }

    kfree(io_buf);
    return -1; // 真正的全满了
}

static int32_t get_or_alloc_index(struct super_block* sb, uint32_t* parent_block_ptr, bool* is_new_block) {
    if (*parent_block_ptr != 0) {
        *is_new_block = false;
        return *parent_block_ptr;
    }

    // 分配新的索引块
    int32_t new_idx_phys = ext2_resource_alloc(sb, 0, EXT2_BLOCK_BITMAP);
    if (new_idx_phys == -1) return -1;

    // 初始化新块（必须清零，防止指向随机地址）
    void* empty_block = kmalloc(sb->s_block_size);
    memset(empty_block, 0, sb->s_block_size);
    partition_write(get_part_by_rdev(sb->s_dev), BLOCK_TO_SECTOR(sb, new_idx_phys), empty_block, sb->s_block_size / SECTOR_SIZE);
    kfree(empty_block);

    *parent_block_ptr = new_idx_phys;
    *is_new_block = true;
    return new_idx_phys;
}

int32_t ext2_append_block_to_inode(struct inode* inode, uint32_t phys_block) {
    struct super_block* sb = inode->i_sb;
    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint32_t bsize = sb->s_block_size;
    uint32_t pnts = bsize / 4; // 每个块可以存多少个指针
    // 计算当前新块将占用的块号
    uint32_t logical_idx = inode->i_size / bsize; 
    
    uint32_t* buf = kmalloc(bsize);
    int32_t status = 0;

   
    if (logical_idx < 12) { // 处理直接块 (0-11) 
        inode->ext2_i.i_block[logical_idx] = phys_block;
    } else if (logical_idx < 12 + pnts) { // 一级间接 (12 ~ 12+pnts-1) 
        bool is_new;
        int32_t idx_block = get_or_alloc_index(sb, &inode->ext2_i.i_block[12], &is_new);
        if (idx_block == -1) { status = -ENOSPC; goto out; }
        if (is_new) inode->i_blocks += (bsize / SECTOR_SIZE);

        partition_read(part, BLOCK_TO_SECTOR(sb, idx_block), buf, bsize / SECTOR_SIZE);
        buf[logical_idx - 12] = phys_block;
        partition_write(part, BLOCK_TO_SECTOR(sb, idx_block), buf, bsize / SECTOR_SIZE);
    } else if (logical_idx < 12 + pnts + pnts * pnts) { // 二级间接 
        uint32_t rel_idx = logical_idx - (12 + pnts);
        uint32_t level1_idx = rel_idx / pnts;
        uint32_t level2_idx = rel_idx % pnts;

        // 获取/分配顶级索引块 (i_block[13])
        bool is_new;
        int32_t top_idx = get_or_alloc_index(sb, &inode->ext2_i.i_block[13], &is_new);
        if (top_idx == -1) { status = -ENOSPC; goto out; }
        if (is_new) inode->i_blocks += (bsize / SECTOR_SIZE);

        // 获取/分配第二级索引块
        partition_read(part, BLOCK_TO_SECTOR(sb, top_idx), buf, bsize / SECTOR_SIZE);
        int32_t sec_idx = get_or_alloc_index(sb, &buf[level1_idx], &is_new);
        if (sec_idx == -1) { status = -ENOSPC; goto out; }
        if (is_new) {
            inode->i_blocks += (bsize / SECTOR_SIZE);
            partition_write(part, BLOCK_TO_SECTOR(sb, top_idx), buf, bsize / SECTOR_SIZE); // 更新顶级块里的指针
        }

        // 写入最终的数据块指针
        partition_read(part, BLOCK_TO_SECTOR(sb, sec_idx), buf, bsize / SECTOR_SIZE);
        buf[level2_idx] = phys_block;
        partition_write(part, BLOCK_TO_SECTOR(sb, sec_idx), buf, bsize / SECTOR_SIZE);
    } else { // 三级间接
        uint32_t pnts2 = pnts * pnts; // 二级能容纳的指针总数
        uint32_t rel_idx = logical_idx - (12 + pnts + pnts2);
        
        uint32_t level1_idx = rel_idx / pnts2; // 在 i_block[14] 里的索引
        uint32_t level2_idx = (rel_idx % pnts2) / pnts; // 在二级块里的索引
        uint32_t level3_idx = rel_idx % pnts; // 在三级块里的索引

        bool is_new;
        // 获取/分配顶级块 (i_block[14])
        int32_t top_idx = get_or_alloc_index(sb, &inode->ext2_i.i_block[14], &is_new);
        if (top_idx == -1) { status = -ENOSPC; goto out; }
        if (is_new) inode->i_blocks += (bsize / SECTOR_SIZE);

        // 获取/分配二级块
        partition_read(part, BLOCK_TO_SECTOR(sb, top_idx), buf, bsize / SECTOR_SIZE);
        int32_t mid_idx = get_or_alloc_index(sb, &buf[level1_idx], &is_new);
        if (mid_idx == -1) { status = -ENOSPC; goto out; }
        if (is_new) {
            inode->i_blocks += (bsize / SECTOR_SIZE);
            partition_write(part, BLOCK_TO_SECTOR(sb, top_idx), buf, bsize / SECTOR_SIZE);
        }

        // 获取/分配三级块
        partition_read(part, BLOCK_TO_SECTOR(sb, mid_idx), buf, bsize / SECTOR_SIZE);
        int32_t bot_idx = get_or_alloc_index(sb, &buf[level2_idx], &is_new);
        if (bot_idx == -1) { status = -ENOSPC; goto out; }
        if (is_new) {
            inode->i_blocks += (bsize / SECTOR_SIZE);
            partition_write(part, BLOCK_TO_SECTOR(sb, mid_idx), buf, bsize / SECTOR_SIZE);
        }

        // 写入最终的数据块指针
        partition_read(part, BLOCK_TO_SECTOR(sb, bot_idx), buf, bsize / SECTOR_SIZE);
        buf[level3_idx] = phys_block;
        partition_write(part, BLOCK_TO_SECTOR(sb, bot_idx), buf, bsize / SECTOR_SIZE);
        
        // 判定是否超出文件系统支持的最大范围
        // (对于 1KB 块，3级上限约 16GB；4KB 块，3级上限约 4TB)
        if (level1_idx >= pnts) { status = -EFBIG; goto out; }
    }

    if (status == 0) {
        // 物理块确实被占用了，所以i_blocks要更新
        // 但是由于我们还没有write数据，因此i_size不更新
        // i_size交给write和mkdir等上层操作来更新
        // inode->i_size += bsize;
        inode->i_blocks += (bsize / SECTOR_SIZE);
    }

out:
    kfree(buf);
    return status;
}

static int32_t ext2_add_entry(struct inode* dir, uint32_t inode_no,char* name, uint8_t file_type) {
    uint32_t block_size = dir->i_sb->s_block_size;
    uint32_t name_len = strlen(name);
    uint32_t needed_len = EXT2_DIR_REC_LEN(name_len);
    struct partition* part = get_part_by_rdev(dir->i_dev);
    void* io_buf = kmalloc(block_size);

    // 计算当前目录总共有多少个逻辑块
    // 目录的 i_size 必须是 block_size 的倍数
    uint32_t total_blocks = dir->i_size / block_size;

    // 遍历所有逻辑块 (0, 1, 2, ..., 直到三级间接能达到的上限)
    for (uint32_t i = 0; i < total_blocks; i++) {
        // 使用 ext2_bmap 获取物理块号
        uint32_t phys_block = ext2_bmap(dir, i);
        
        // 如果 phys_block 为 0，说明遇到了空洞（在目录中理论上不应出现，除非文件系统损坏），我们先直接跳过
        if (phys_block == 0) continue; 

        partition_read(part, BLOCK_TO_SECTOR(dir->i_sb, phys_block), io_buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry* de = (struct ext2_dir_entry*)io_buf;
        uint32_t offset = 0;

        while (offset < block_size) {
            uint32_t actual_len = EXT2_DIR_REC_LEN(de->name_len);

            // 检查剩余空间
            if (de->rec_len - actual_len >= needed_len) {
                // 找到空隙，开始建立新项
                uint16_t old_total_len = de->rec_len;
                de->rec_len = actual_len;

                // 指向新项
                de = (struct ext2_dir_entry*)((char*)de + actual_len);
                de->i_no = inode_no;
                // 当前这个新的目录项直接吃下这一块区域的所有剩余空间
                // 防止出现空洞
                de->rec_len = old_total_len - actual_len;
                de->name_len = name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);

                // 写回物理地址
                partition_write(part, BLOCK_TO_SECTOR(dir->i_sb, phys_block), io_buf, block_size / SECTOR_SIZE);
                kfree(io_buf);
                return 0;
            }
            offset += de->rec_len;
            de = (struct ext2_dir_entry*)((char*)io_buf + offset);
        }
    }

    // 如果运行到这里，说明现有块全满了，需要开新块
    // 分配一个新的物理块
    int32_t new_phys_block = ext2_resource_alloc(dir->i_sb, 0, EXT2_BLOCK_BITMAP); 
    if (new_phys_block == -1) {
        kfree(io_buf);
        PANIC("fail to ext2_resource_alloc");
        return -ENOSPC;
    }

    // 将新块挂载到 inode 的逻辑结构中
    // 这一步会更新 i_block[12/13/14] 以及 i_size
    if (ext2_append_block_to_inode(dir, new_phys_block) < 0) {
        // 回滚...
        PANIC("fail to ext2_append_block_to_inode");
        kfree(io_buf);
        return -EIO;
    }
    // 目录至少占用一个完整块
    dir->i_size += dir->i_sb->s_block_size;          

    // 初始化这个新块为一个巨大的目录项空位
    memset(io_buf, 0, block_size);
    struct ext2_dir_entry* new_de = (struct ext2_dir_entry*)io_buf;
    new_de->i_no = inode_no;
    new_de->rec_len = block_size; // 这个块目前只有一个项，且占满全块
    new_de->name_len = name_len;
    new_de->file_type = file_type;
    memcpy(new_de->name, name, name_len);

    partition_write(part, BLOCK_TO_SECTOR(dir->i_sb, new_phys_block), io_buf, block_size / SECTOR_SIZE);

    kfree(io_buf);
    return 0;
}

static void ext2_init_dir_block(struct super_block* sb, uint32_t block_addr, uint32_t self_ino, uint32_t parent_ino) {
    uint32_t block_size = sb->s_block_size;
    void* io_buf = kmalloc(block_size);
    memset(io_buf, 0, block_size);

    // 初始化 "." (固定占 12 字节)
    // 12字节是由于ext2_dir_entry本身站8字节，然后名称 . 占一字节，所以一共9字节
    // 但是要求4字节对齐，因此对齐到12字节 
    struct ext2_dir_entry* de = (struct ext2_dir_entry*)io_buf;
    de->i_no = self_ino;
    de->name_len = 1;
    de->file_type = FT_DIRECTORY;
    de->rec_len = 12; // 8字节头 + 1字节名 + 3字节对齐 = 12
    memcpy(de->name, ".", 1);

    // 初始化 ".." (跳过 12 字节，占据剩余全部)
    // 这么做是为了将 .. 作为终止符
    // 由于我们遍历目录项时，是通过 cursor += rec_len 的方式来寻找下一个目录项的
    // 为了避免会读到块后面的垃圾数据，因此我们让最后一个目录项直接占用到块的末尾
    // 这样 cursor += rec_len 就会直接跳出块缓冲区外，不会读到垃圾数据
    // Ext2 添加新文件（add_entry）的逻辑是
    // 寻找一个“实际长度”小于“记录长度（rec_len）”的项，然后把它拆开。
    // 假设在要在新目录下创建一个文件 test.txt（需要 16 字节）
    // 内核读取该块，先看 .：rec_len 是 12，实际也是 12，没空间。
    // 再看 ..：它的 rec_len 是 1012（1024 - 12），但它的实际需求只有 12 字节。
    // 此时拆分发生，内核把 .. 的 rec_len 缩小为 12
    // 然后在偏移 24 字节的地方写入 test.txt
    // 并把剩下的 1012 - 12 = 1000 字节给 test.txt 的 rec_len。
    // 检查一个目录项实际长度的操作由宏 EXT2_DIR_REC_LEN 来进行
    // 这也是为什么他要保证4字节对齐的原因，因为目录项实际长度都是4字节对齐的
    // 同理，删除文件的时候也要将当前目录项的空间与前一个目录项的rec_len合并
    de = (struct ext2_dir_entry*)((char*)de + 12);
    de->i_no = parent_ino;
    de->name_len = 2;
    de->file_type = FT_DIRECTORY;
    de->rec_len = block_size - 12; // 吞掉剩下的所有空间
    memcpy(de->name, "..", 2);

    // 写入磁盘
    struct partition* part = get_part_by_rdev(sb->s_dev);
    partition_write(part, BLOCK_TO_SECTOR(sb, block_addr), io_buf, block_size / SECTOR_SIZE);
    kfree(io_buf);
}

static void ext2_inode_init(struct partition* part, uint32_t inode_no,struct inode* new_inode,enum file_types ft, int mode){
    memset(new_inode,0,sizeof(struct inode));
    // VFS 基础字段
    // 没有被显示初始化的字段都是0，都已经被上面的 memset 初始化过了
	new_inode->i_no = inode_no;
	new_inode->i_type = ft;
	new_inode->i_dev = part->i_rdev;
    new_inode->i_sb = part->sb; // 建立归属超级块，以后读写数据块要用到他
    new_inode->write_deny = false;

	// 初始化ext2专用字段
    new_inode->i_mode = encode_imode(ft,mode);
    if(ft == FT_DIRECTORY){
        // 初始计数为2，例如d/dd，dd目录在创建时的初始计数为2，这是因为d下有一个目录项d计数了一次
        // dd自己下面还有一个目录项 . 又计数了一次
        new_inode->ext2_i.i_links_count = 2;
        // 只有目录创建时会默认分配一个块，所以 size 是 block_size
        new_inode->i_size = part->sb->s_block_size;
        // 与 i_size 保持一致，只不过单位是扇区数
        new_inode->i_blocks = part->sb->s_block_size / SECTOR_SIZE;
    }else{
        // 其他文件由于没有指向自己的 .
        // 因此结果是 1
        new_inode->ext2_i.i_links_count = 1;
        new_inode->i_size = 0;
        new_inode->i_blocks = 0; // 普通文件或设备文件初始没数据块
    }

    switch (ft) {
        case FT_REGULAR:
            new_inode->i_op = &ext2_file_inode_operations;
            break;
        case FT_DIRECTORY:
            new_inode->i_op = &ext2_dir_inode_operations;
            break;
        case FT_CHAR_SPECIAL:
            new_inode->i_op = &ext2_chardev_inode_operations;
            break;
        case FT_BLOCK_SPECIAL:
            new_inode->i_op = &ext2_blkdev_inode_operations;
            break;
        case FT_SYMLINK:
            new_inode->i_op = &ext2_symlink_inode_operations;
            break;
        default:
            new_inode->i_op = NULL;
            break;
    }

}

// 创建目录文件
static int32_t ext2_mkdir(struct inode* dir,char* name, int len, int mode)  {
    struct super_block* sb = dir->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(sb->s_dev);
    // int len = strlen(name);

    if (len >= MAX_FILE_NAME_LEN) return -ENAMETOOLONG; // 暂时保护固定长度的 dirent 

    // 分配资源
    uint32_t group = (dir->i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    
    int32_t inode_no = ext2_resource_alloc(sb, group, EXT2_INODE_BITMAP);
    if (inode_no == -1){
        PANIC("ext2_mkdir: fail to ext2_resource_alloc for inode_no\n");
        return -ENOSPC;
    }

    int32_t block_addr = ext2_resource_alloc(sb, group, EXT2_BLOCK_BITMAP);
    if (block_addr == -1) {
        PANIC("ext2_mkdir: fail to ext2_resource_alloc for block_addr\n");
        // 回滚 inode ，目前先简单PANIC
        return -ENOSPC;
    }

    // 在内存中准备新 Inode
    struct inode* new_inode = (struct inode*)kmalloc(sizeof(struct inode));
    
    ext2_inode_init(part, inode_no, new_inode,FT_DIRECTORY,mode); 
    
    new_inode->ext2_i.i_block[0] = block_addr; // 第一个数据块

    // 初始化新目录的数据块 (. 和 ..)
    ext2_init_dir_block(sb, block_addr, inode_no, dir->i_no);

    // 获取 64 位时间戳并转为磁盘需要的 32 位
    uint32_t now = (uint32_t)sys_time();

    // 初始化新目录的 inode 时间
    new_inode->i_atime = now;
    new_inode->i_mtime = now;
    new_inode->i_ctime = now;
    new_inode->ext2_i.i_dtime = 0;

    // 将新目录挂载到父目录的数据块中
    if (ext2_add_entry(dir, inode_no, name, FT_DIRECTORY) < 0) {
        // 回滚逻辑...
        PANIC("ext2_mkdir: fail to ext2_add_entry\n");
        return -EIO;
    }

    // 更新元数据账本
    // 更新父目录
    dir->ext2_i.i_links_count++; // 因为子目录有 ".." 指向父目录

    dir->i_mtime = now;   // 目录内容变了
    dir->i_ctime = now;   // 链接数和 mtime 变了，所以 ctime 必变
    
    // 更新目录数量
    ext2_info->group_desc[group].bg_used_dirs_count++;

    // 同步到磁盘
    sb->s_op->write_inode(new_inode);
    sb->s_op->write_inode(dir);
    ext2_sync_gdt(sb);
    sb->s_op->write_super(sb);

    kfree(new_inode);
    return 0;
}

static int32_t ext2_bmap(struct inode* inode, int32_t _index) {
    // 为了对齐接口，参数我们写成了int32_t类型，但是该函数全当无符号类型用 
    uint32_t index = (uint32_t) _index; 
    struct super_block* sb = inode->i_sb;
    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    uint32_t pnts_per_block = block_size / 4; // 每个块能容纳的指针数
    uint32_t* buf = kmalloc(block_size);
    uint32_t phys_block = 0;

    if (!buf) return 0;

    // 直接块 (0 - 11)
    if (index < 12) {
        phys_block = inode->ext2_i.i_block[index];
        goto out;
    }
    index -= 12;

    // 一级间接块 (12)
    if (index < pnts_per_block) {
        // 索引块都不存在，直接返回0
        if (inode->ext2_i.i_block[12] == 0) return 0; 
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[12]), buf, block_size/SECTOR_SIZE);
        phys_block = buf[index];
        goto out;
    }
    index -= pnts_per_block;

    // 二级间接块 (13)
    if (index < pnts_per_block * pnts_per_block) {
        // 读取一级索引表（顶级）
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[13]), buf, block_size/SECTOR_SIZE);
        uint32_t first_idx = index / pnts_per_block;
        uint32_t second_idx = index % pnts_per_block;
        
        // 读取二级索引表
        partition_read(part, BLOCK_TO_SECTOR(sb, buf[first_idx]), buf, block_size/SECTOR_SIZE);
        phys_block = buf[second_idx];
        goto out;
    }
    index -= (pnts_per_block * pnts_per_block);

    // 三级间接块 (14) 
    // 最大范围可达 pnts_per_block^3，对于 1KB 块就是 1^3 = 64MB (太小)，但对于 4KB 块就是 1024^3 = 4TB
    uint32_t pnts_per_level2 = pnts_per_block * pnts_per_block;
    if (index < pnts_per_level2 * pnts_per_block) {
        if (inode->ext2_i.i_block[14] == 0) return 0;
        // 读取一级索引表
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[14]), buf, block_size/SECTOR_SIZE);
        uint32_t first_idx = index / pnts_per_level2;
        uint32_t remain = index % pnts_per_level2;

        // 读取二级索引表
        uint32_t next_block = buf[first_idx];
        //  如果发现中途某个块地址为 0（即 next_block == 0），说明文件存在“空洞”（Sparse File），此时应该直接返回 0。
        if (next_block == 0) goto out; 
        partition_read(part, BLOCK_TO_SECTOR(sb, next_block), buf, block_size/SECTOR_SIZE);
        uint32_t second_idx = remain / pnts_per_block;
        uint32_t third_idx = remain % pnts_per_block;

        // 读取三级索引表
        next_block = buf[second_idx];
        //  如果发现中途某个块地址为 0（即 next_block == 0），说明文件存在“空洞”（Sparse File），此时应该直接返回 0。
        if (next_block == 0) goto out;
        partition_read(part, BLOCK_TO_SECTOR(sb, next_block), buf, block_size/SECTOR_SIZE);
        phys_block = buf[third_idx];
        goto out;
    }

out:
    kfree(buf);
    return phys_block;
}

static int32_t ext2_create(struct inode* dir, char* name, int len, int mode) {
    struct super_block* sb = dir->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(sb->s_dev);

    if (len >= MAX_FILE_NAME_LEN) return -ENAMETOOLONG;

    // 分配 Inode 编号
    // 从父目录所在的块组开始分配，找不到再去其他的，以便提升局部性
    uint32_t preferred_group = (dir->i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    int32_t inode_no = ext2_resource_alloc(sb, preferred_group, EXT2_INODE_BITMAP);
    if (inode_no == -1) {
        return -ENOSPC;
    }

    // 初始化内存中的 Inode 结构
    struct inode* new_file_inode = (struct inode*)kmalloc(sizeof(struct inode));
    if (!new_file_inode) {
        PANIC("fail to kmalloc for new_file_inode");
        return -ENOMEM;
    }

    ext2_inode_init(part, inode_no, new_file_inode, FT_REGULAR, mode);

    int64_t now = sys_time(); 

    // 初始化新文件的 inode 时间
    new_file_inode->i_atime = (uint32_t)now;
    new_file_inode->i_mtime = (uint32_t)now;
    new_file_inode->i_ctime = (uint32_t)now;
    new_file_inode->ext2_i.i_dtime = 0;

    // 在父目录的数据块中添加目录项
    if (ext2_add_entry(dir, inode_no, name, FT_REGULAR) < 0) {
        // 回滚逻辑
        kfree(new_file_inode);
        PANIC("fail to ext2_add_entry");
        return -EIO;
    }

    // 更新父目录的修改时间和状态改变时间
    dir->i_mtime = (uint32_t)now;
    dir->i_ctime = (uint32_t)now;

    // 持久化元数据
    // 写入新创建文件的 inode 到磁盘上的 Inode Table
    sb->s_op->write_inode(new_file_inode);

    // 写入更新后的父目录 inode（因为 add_entry 可能会增加父目录的 i_size 或 i_mtime）
    sb->s_op->write_inode(dir);

    // 同步组描述符表 (GDT)，因为 resource_alloc 减少了该组的空闲 inode 计数
    ext2_sync_gdt(sb);

    // 同步超级块 (因为总空闲 inode 计数变了)
    sb->s_op->write_super(sb);

    // 清理并返回
    // 最后返回 inode 号，如果用户要使用就走 inode_open 的逻辑重新打开
    kfree(new_file_inode);
    return inode_no;
}

static int ext2_lookup(struct inode* dir, char* name, int len, struct inode** res) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    struct super_block* sb = dir->i_sb;
    
    // 确定 Block Size
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    
    // 计算该目录文件一共有多少个逻辑块
    // 目录的大小由 i_size 决定，我们需要遍历所有逻辑块
    uint32_t total_blocks = (dir->i_size + block_size - 1) / block_size;

    char* buf = (char*)kmalloc(block_size);
    if (!buf) {
        put_str("ext2_lookup: fail to kmalloc for buf\n");
        return -ENOMEM;
    }

    // 使用bmap遍历所有逻辑块
    for (uint32_t i = 0; i < total_blocks; i++) {
        uint32_t phys_block = ext2_bmap(dir, i);
        
        // 如果返回 0，说明这个逻辑块是个空洞（在目录中很少见，但需处理）
        if (phys_block == 0) continue;

        // 读取物理块数据
        partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry* de = (struct ext2_dir_entry*)buf;
        uint32_t offset = 0;

        // 在当前块内遍历所有目录项
        while (offset < block_size) {
            // 匹配条件是 Inode 不为0 且 长度匹配 且 内容匹配
            if (de->i_no != 0 && de->name_len == len && 
                memcmp(name, de->name, len) == 0) {
                
                // 找到编号，利用 VFS 的 inode_open 打开它
                struct inode* target_inode = inode_open(part, de->i_no);
                if (!target_inode) {
                    kfree(buf);
                    return -EIO;
                }

                *res = target_inode;
                kfree(buf);
                return 0; // 成功找到
            }

            // 异常保护：防止死循环
            if (de->rec_len == 0) {
                put_str("ext2_lookup: dir_entry rec_len is 0, disk corrupted!\n");
                break;
            }

            // 按 rec_len 跳转（Ext2 中的目录项是可以变长的）
            offset += de->rec_len;
            de = (struct ext2_dir_entry*)((uint8_t*)buf + offset);
        }
    }

    // 遍历完所有块都没找到
    kfree(buf);
    *res = NULL;
    return -ENOENT; 
}

int32_t ext2_inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count) {
    struct partition* part = get_part_by_rdev(inode->i_dev);
    struct super_block* sb = inode->i_sb;
    uint32_t block_size = sb->s_block_size;
    
    // 边界检查
    uint32_t size = count;
    if (offset + count > inode->i_size) {
        size = inode->i_size - offset;
    }
    if (size <= 0) return 0;

    // 准备缓冲区（用于处理非块对齐的起始/结束位置）
    uint8_t* io_buf = kmalloc(block_size);
    if (!io_buf) return -ENOMEM;

    uint32_t bytes_read = 0;
    uint32_t curr_pos = offset;
    uint8_t* dst = (uint8_t*)buf;

    while (bytes_read < size) {
        uint32_t logical_idx = curr_pos / block_size;
        uint32_t offset_in_block = curr_pos % block_size;
        
        uint32_t space_in_block = block_size - offset_in_block;
        uint32_t chunk_size = (size - bytes_read < space_in_block) ? (size - bytes_read) : space_in_block;

        // 使用 bmap 核心函数获取物理块号
        uint32_t phys_block = inode->i_op->bmap(inode, logical_idx);

        if (phys_block == 0) {
            // Ext2 支持空洞文件，如果物理块不存在，按规定填充 0
            memset(dst, 0, chunk_size);
        } else {
            // 读取整个块
            partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, block_size / SECTOR_SIZE);
            // 只拷贝我们需要的那一部分（处理 offset 不是块对齐的情况）
            memcpy(dst, io_buf + offset_in_block, chunk_size);
        }

        bytes_read += chunk_size;
        curr_pos += chunk_size;
        dst += chunk_size;
    }

    kfree(io_buf);
    return bytes_read;
}

static int32_t ext2_mknod(struct inode* dir, char* name, int len, int type, int dev) {
    struct super_block* sb = dir->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(sb->s_dev);

    // 检查文件名长度
    if (len >= MAX_FILE_NAME_LEN) return -ENAMETOOLONG;

    // 分配 Inode 编号
    uint32_t group = (dir->i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    int32_t inode_no = ext2_resource_alloc(sb, group, EXT2_INODE_BITMAP);
    ASSERT(inode_no != -1);
    if (inode_no == -1) return -ENOSPC;

    // 在内存中初始化新 Inode
    struct inode* new_inode = (struct inode*)kmalloc(sizeof(struct inode));
    ASSERT(new_inode!=NULL);
    if (!new_inode) return -ENOMEM;

    ext2_inode_init(part, inode_no, new_inode, type, 0777);

    // 设备号存储
    new_inode->i_size = 0;           // 设备文件逻辑大小为 0
    new_inode->i_blocks = 0;   // 不占用数据块
    new_inode->i_rdev = dev;         // 内存结构中的设备号
    
    // 在 Ext2 磁盘结构中，通常将设备号存放在 i_block[0]
    new_inode->ext2_i.i_block[0] = dev; 

    uint32_t now = (uint32_t)sys_time();

    // 初始化新设备文件的 Inode 时间
    // 即使是设备文件，这些字段在磁盘镜像 struct ext2_inode 中也是存在的
    new_inode->i_atime = now;
    new_inode->i_mtime = now;
    new_inode->i_ctime = now;
    new_inode->ext2_i.i_dtime = 0;

    // 将新 Inode 挂载到父目录的数据块中
    if (ext2_add_entry(dir, inode_no, name, type) < 0) {
        // 这里理论上需要回滚 inode_bitmap，目前先简单处理
        PANIC("fail to ext2_add_entry");
        kfree(new_inode);
        return -EIO;
    }

    // 更新父目录时间
    dir->i_mtime = now;
    dir->i_ctime = now;

    // 同步元数据到磁盘
    sb->s_op->write_inode(new_inode); // 写入新 inode
    sb->s_op->write_inode(dir); // 写入父目录 inode (因为 i_size 或内容变了)
    
    ext2_sync_gdt(sb);
    sb->s_op->write_super(sb);

    kfree(new_inode);
    return 0;
}

static void ext2_resource_free(struct super_block *sb, uint32_t index, enum ext2_bitmap_type type) {
    struct ext2_sb_info *ext2_info = &sb->ext2_info;
    struct partition *part = get_part_by_rdev(sb->s_dev);
    
    uint32_t group;
    uint32_t bit_idx;
    uint32_t btmp_block;
    uint32_t bits_per_group;

    // 计算该资源属于哪个组以及组内偏移
    if (type == EXT2_INODE_BITMAP) {
        // Inode 编号从 1 开始
        uint32_t real_idx = index - 1;
        bits_per_group = ext2_info->sb_raw.s_inodes_per_group;
        group = real_idx / bits_per_group;
        bit_idx = real_idx % bits_per_group;
        btmp_block = ext2_info->group_desc[group].bg_inode_bitmap;
    } else {
        // Block 编号从 s_first_data_block 开始
        uint32_t real_idx = index - ext2_info->sb_raw.s_first_data_block;
        bits_per_group = ext2_info->sb_raw.s_blocks_per_group;
        group = real_idx / bits_per_group;
        bit_idx = real_idx % bits_per_group;
        btmp_block = ext2_info->group_desc[group].bg_block_bitmap;
    }

    // printk("Freeing block %d in group %d\n", index, group);

    // 读取并修改位图
    uint8_t* io_buf = kmalloc(sb->s_block_size);
    if (!io_buf) {
        PANIC("fail to kmalloc for io_buf");
        return; 
    }

    partition_read(part, BLOCK_TO_SECTOR(sb, btmp_block), io_buf, sb->s_block_size / SECTOR_SIZE);
    
    struct bitmap btmp = { .btmp_bytes_len = bits_per_group / 8, .bits = io_buf };
    
    // 检查是否已经是 0
    if (bitmap_bit_check(&btmp, bit_idx) == 0) {
        // 已经释放过了，或者索引计算错误
        kfree(io_buf);
        return; 
    }

    bitmap_set(&btmp, bit_idx, 0); // 清零
    partition_write(part, BLOCK_TO_SECTOR(sb, btmp_block), io_buf, sb->s_block_size / SECTOR_SIZE);

    // 更新统计数据
    if (type == EXT2_INODE_BITMAP) {
        ext2_info->group_desc[group].bg_free_inodes_count++;
        ext2_info->sb_raw.s_free_inodes_count++;
    } else {
        ext2_info->group_desc[group].bg_free_blocks_count++;
        ext2_info->sb_raw.s_free_blocks_count++;
    }

    // 这里只是改了内存里的描述符计数，
    // 因此在 unlink 的末尾要统一 sync_gdt 和 write_super
    kfree(io_buf);
}

// recursive_free_blocks 用于递归释放索引块
// level 是块层级，0-数据块, 1-一级间接, 2-二级, 3-三级
// phys_block 是当前处理的物理块号
// first_logic_idx 是当前处理的物理块在文件里对应的第一个逻辑块号
// free_start_idx 是用户要求从哪个逻辑块号开始真正释放
static void recursive_free_blocks(struct inode *inode, uint32_t phys_block, uint8_t level, uint32_t first_logic_idx, uint32_t free_start_idx) {
    if (phys_block == 0) return;

    struct super_block* sb = inode->i_sb;
    struct partition *part = get_part_by_rdev(sb->s_dev);
    uint32_t bsize = sb->s_block_size;
    uint32_t pnts = bsize / 4;

    if (level > 0) {
        uint32_t *buf = kmalloc(bsize);
        if (!buf){
            PANIC("fail to kmalloc for buf");
            return;
        } 

        partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), buf, bsize / SECTOR_SIZE);

        for (uint32_t i = 0; i < pnts; i++) {
            if (buf[i] != 0) {
                // 计算子块的逻辑起始编号
                // level 1 (一级间接): 每个子块就是 1 个逻辑块
                // level 2 (二级间接): 每个子块代表 pnts 个逻辑块
                // level 3 (二级间接): 每个子块代表 pnts*pnts 个逻辑块
                uint32_t child_step = 1;
                for(int l=0; l < level-1; l++) child_step *= pnts;
                
                uint32_t child_logic_idx = first_logic_idx + i * child_step;
                
                // 只有当这个子块覆盖的范围可能超过 free_start_idx 时才递归
                if (child_logic_idx + child_step > free_start_idx) {
                    recursive_free_blocks(inode, buf[i], level - 1, child_logic_idx, free_start_idx);
                    
                    // 如果整个子块范围都被释放了，清理索引项
                    if (child_logic_idx >= free_start_idx) {
                        buf[i] = 0;
                    }
                }
            }
        }
        // 如果是间接块自己也要释放（前提是它的逻辑范围全在 free_start_idx 之后）
        // 如果该索引块负责的所有内容都被删了，这里写回 0
        partition_write(part, BLOCK_TO_SECTOR(sb, phys_block), buf, bsize / SECTOR_SIZE);
        kfree(buf);
    }

    // 释放物理块的条件, 它的逻辑起始编号 >= 截断点
    // 如果是间接块(level > 0)，释放前提是它下面的内容全被清空了
    if (first_logic_idx >= free_start_idx) {
        ext2_resource_free(sb, phys_block, EXT2_BLOCK_BITMAP);
        // 修改 inode 的块统计信息
        // Ext2 中 i_blocks 的单位是 512B 扇区
        uint32_t sectors_per_block = bsize / SECTOR_SIZE;
        if (inode->i_blocks >= sectors_per_block) {
            inode->i_blocks -= sectors_per_block;
        } else {
            inode->i_blocks = 0;
        }
    }
}

// 根据传入进来的 inode 的大小 i_size 进行裁剪
// 就把大小裁剪到 i_size
static void ext2_truncate(struct inode *inode) {
    struct super_block *sb = inode->i_sb;
    uint32_t bsize = sb->s_block_size;
    uint32_t pnts = bsize / 4; // 一个块可以存储几个块号

    // 计算从哪个逻辑块号开始释放
    // 例如 size=1500, bsize=1024, 则索引 0,1 保留, 从索引 2 开始释放
    uint32_t free_start_idx = (inode->i_size + bsize - 1) / bsize;

    // 处理对齐清零，如果新大小不在块边界，清空最后一个保留块的剩余部分
    // 比如上面的 size = 1500，我们需要将第 1 个块之后空闲出来的 548 字节的剩余部分清零
    uint32_t partial_offset = inode->i_size % bsize; // 计算块内偏移 1500 % 1024 = 476
    if (partial_offset != 0) { // 处理不是块对齐的情况
        uint32_t last_logic_idx = inode->i_size / bsize;
        // 通过逻辑块号获取物理块号
        // 按理来说这个 inode 应该是有 bmap 函数的，如果没有的话会报空指针，到时候再来处理
        uint32_t phys_block = inode->i_op->bmap(inode, last_logic_idx); 
        
        if (phys_block != 0) {
            uint8_t *io_buf = kmalloc(bsize);
            if (io_buf) {
                struct partition *part = get_part_by_rdev(sb->s_dev);
                partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, bsize / SECTOR_SIZE);
                
                // 将 new_size 之后的部分全部抹零，比如前面的例子中，就是将 476 字节之后的 548 字节清空
                memset(io_buf + partial_offset, 0, bsize - partial_offset);
                
                partition_write(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, bsize / SECTOR_SIZE);
                kfree(io_buf);
            }
        }
    }

    // 执行递归释放（内部会修改 i_blocks）
    // 直接块
    for (uint32_t i = 0; i < 12; i++) {
        if (i >= free_start_idx && inode->ext2_i.i_block[i] != 0) {
            recursive_free_blocks(inode, inode->ext2_i.i_block[i], 0, i, free_start_idx);
            inode->ext2_i.i_block[i] = 0;
        }
    }

    // 一级间接块 (逻辑范围 12 ~ 12+pnts-1)
    if (inode->ext2_i.i_block[12] != 0 && 12 + pnts > free_start_idx) {
        recursive_free_blocks(inode, inode->ext2_i.i_block[12], 1, 12, free_start_idx);
        if (free_start_idx <= 12) inode->ext2_i.i_block[12] = 0;
    }

    // 二级间接块 (逻辑范围从 12+pnts 开始)
    uint32_t l2_start = 12 + pnts;
    if (inode->ext2_i.i_block[13] != 0 && l2_start + pnts*pnts > free_start_idx) {
        recursive_free_blocks(inode, inode->ext2_i.i_block[13], 2, l2_start, free_start_idx);
        if (free_start_idx <= l2_start) inode->ext2_i.i_block[13] = 0;
    }

    // 三级间接块覆盖的范围是 [l3_start, l3_start + pnts^3 - 1]
    uint32_t l3_start = 12 + pnts + (pnts * pnts);
    if (inode->ext2_i.i_block[14] != 0) {
        // 如果截断点落在三级间接块范围内，或者三级间接块整个都在截断点后
        // 我们计算三级间接块的总覆盖量：pnts^3。这里简单判断下界即可。
        if (free_start_idx < l3_start + (uint64_t)pnts * pnts * pnts) {
            recursive_free_blocks(inode, inode->ext2_i.i_block[14], 3, l3_start, free_start_idx);
            
            // 如果整个三级间接块都被释放了
            if (free_start_idx <= l3_start) {
                inode->ext2_i.i_block[14] = 0;
            }
        }
    }

    // 重置元数据的操作不应该在此处做，i_size 应该是在上层的 sys_truncate 函数中
    // 根据用户提供的值来修改的，i_blocks 是在 recursive_free_blocks 中一边改一遍修改的
    // inode->i_size = 0;
    // inode->i_blocks = 0;
    
    // 更新磁盘上的 Inode 结构
    sb->s_op->write_inode(inode);
}

static int32_t ext2_remove_entry(struct inode* parent_dir, char* name) {
    struct partition* part = get_part_by_rdev(parent_dir->i_dev);
    struct super_block* sb = parent_dir->i_sb;
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    
    void* io_buf = kmalloc(block_size);
    if (!io_buf) return -1;

    // 根据 i_size 计算一共有多少个逻辑块
    uint32_t total_logic_blocks = DIV_ROUND_UP(parent_dir->i_size, block_size);

    // 遍历每一个逻辑块
    for (uint32_t i = 0; i < total_logic_blocks; i++) {
        // 使用 bmap 找到物理块 LBA
        uint32_t phys_block_lba = ext2_bmap(parent_dir, i);
        // 处理目录中的空洞
        // 虽然目录很少有空洞，因为目录他会合并目录项，通常不会产生空洞
        if (phys_block_lba == 0) continue; 

        // 将物理块号转换为扇区地址读取
        uint32_t sec_lba = BLOCK_TO_SECTOR(sb, phys_block_lba);
        memset(io_buf, 0, block_size);
        partition_read(part, sec_lba, io_buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry* prev = NULL;
        struct ext2_dir_entry* curr = (struct ext2_dir_entry*)io_buf;
        uint32_t offset = 0;

        // 在块内查找并合并条目
        while (offset < block_size) {
            // 记录当前 rec_len 备用，防止修改 de 后丢失长度
            uint32_t current_rec_len = curr->rec_len;

            // 防止损坏的文件系统导致死循环
            if (current_rec_len == 0) break;

            if (curr->i_no != 0 && strlen(name) == curr->name_len && 
            memcmp(name, curr->name, curr->name_len) == 0) {
                // 命中目标
                if (prev) {
                    // 把当前条目的长度并入前一个条目
                    prev->rec_len += curr->rec_len;
                } else {
                    // 若是块首条目，将 inode 设为 0 标记为无效
                    curr->i_no = 0;
                }

                // 同步回磁盘
                partition_write(part, sec_lba, io_buf, block_size / SECTOR_SIZE);
                
                // 更新父目录的修改时间（不减 i_size，保持目录块预留）
                // 这个逻辑先预留，之后实现了时钟相关的逻辑后再来实现
                // parent_dir->i_mtime = read_rtc(); 

                sb->s_op->write_inode(parent_dir);

                kfree(io_buf);
                return 0; // 成功删除
            }

            // 指针跳跃
            offset += current_rec_len;
            // 已经到头了，不再计算下一个指针
            if (offset >= block_size) break; 

            prev = curr;
            curr = (struct ext2_dir_entry*)((uint8_t*)curr + current_rec_len);
        }
    }

    kfree(io_buf);
    return -1; // 没找到该条目
}

static int32_t ext2_unlink(struct inode* dir, char* name, int len) {
    struct super_block* sb = dir->i_sb;
    // 查找目标 Inode
    struct inode* target_inode = NULL;

    if(dir->i_op->lookup==NULL){
        printk("ext2_unlink: i_no %x is not a dir!\n",dir->i_no);
        return -ENOENT;
    }

    if (dir->i_op->lookup(dir, name, len, &target_inode) != 0) {
        return -ENOENT;
    }

    // 检查是否为目录 (unlink 只能删文件)
    // 在sys_unlink中以及检查过一遍了，这里再拦截一遍
    if (target_inode->i_type == FT_DIRECTORY) {
        inode_close(target_inode);
        return -EISDIR; 
    }

    int64_t now = sys_time();

    // 从父目录删除目录项 (这一步比较复杂，因此给他封装一下)
    // Ext2 的删除需要处理 rec_len 的合并
    if (ext2_remove_entry(dir, name) != 0) {
        inode_close(target_inode);
        return -EIO;
    }

    dir->i_mtime = (uint32_t)now;
    dir->i_ctime = (uint32_t)now;

    // 递减 Inode 链接数
    // 在 Ext2 中，只有 i_links_count == 0 且 i_open_cnts == 0 才会真正释放
    target_inode->ext2_i.i_links_count--;

    target_inode->i_ctime = (uint32_t)now; // 链接数变了，ctime 必变

    if (target_inode->ext2_i.i_links_count == 0) {
        // 记录删除时间
        target_inode->ext2_i.i_dtime = (uint32_t)now;

        // 释放所有关联的磁盘块，直接调用truncate就行
        // 对于设备文件之类的文件，他们没有磁盘块，因此不需要truncate
        // 由于他们的truncate都为NULL，因此这个条件可以将他们拦住
        if(target_inode->i_op!=NULL&&target_inode->i_op->truncate!=NULL){
            // 复用 truncate，里面会根据 i_size 来调整文件大小，此处需要将其置为 0
            target_inode->i_size = 0;
            target_inode->i_op->truncate(target_inode);
        }
        // 释放 Inode 节点，将 Inode Bitmap 相应的位置 0
        ext2_resource_free(sb, target_inode->i_no, EXT2_INODE_BITMAP);
    } else {
        // 如果还有硬链接，只更新元数据
        sb->s_op->write_inode(target_inode);
    }

    // 清理运行时缓存
    // 如果链接数为0，必须从哈希表抹除，防止别人再搜到这个僵尸 inode
    if (target_inode->ext2_i.i_links_count == 0) {
        inode_evict(target_inode);
    } else {
        inode_close(target_inode);
    }

    ext2_sync_gdt(sb);
    sb->s_op->write_super(sb);
    sb->s_op->write_inode(dir);
    
    return 0;
}

// 整体逻辑和sifs里面的类似，只不过sifs里面目录项大小是固定的
// 此处需要通过 rec_len 来跳跃
static bool ext2_is_dir_empty(struct inode *inode) {
    struct super_block *sb = inode->i_sb;
    uint32_t block_size = sb->s_block_size;
    void *io_buf = kmalloc(block_size);
    if (!io_buf) return false;

    // 遍历目录的所有块
    uint32_t total_blocks = DIV_ROUND_UP(inode->i_size, block_size);
    for (uint32_t i = 0; i < total_blocks; i++) {
        uint32_t p_blk = ext2_bmap(inode, i);
        if (p_blk == 0) continue;

        partition_read(get_part_by_rdev(inode->i_dev), BLOCK_TO_SECTOR(sb, p_blk), io_buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry *de = (struct ext2_dir_entry *)io_buf;
        uint32_t offset = 0;
        while (offset < block_size) {
            // 只要找到一个有效且不是 "." 或 ".." 的项，就不为空
            if (de->i_no != 0) {
                if (memcmp(de->name, ".", de->name_len) != 0 && 
                    memcmp(de->name, "..", de->name_len) != 0) {
                    kfree(io_buf);
                    return false;
                }
            }
            offset += de->rec_len;
            de = (struct ext2_dir_entry *)((uint8_t *)de + de->rec_len);
            if (de->rec_len == 0) break;
        }
    }
    kfree(io_buf);
    return true;
}

static int32_t ext2_rmdir(struct inode *dir, char *name, int len) {
    struct super_block *sb = dir->i_sb;
    struct inode *target_inode = NULL;

    // 查找目标目录的 Inode
    if (ext2_lookup(dir, name, len, &target_inode) != 0) {
        return -ENOENT;
    }

    // 检查是否确实是目录
    if (target_inode->i_type != FT_DIRECTORY) {
        inode_close(target_inode);
        return -ENOTDIR;
    }

    // 检查是否为空
    if (!ext2_is_dir_empty(target_inode)) {
        printk("ext2_rmdir: target dir is not empty!\n");
        inode_close(target_inode);
        return -ENOTEMPTY;
    }

    // 从父目录删除目录项
    // 可以直接复用和 unlink 里面一样的
    // ext2_remove_entry，它会处理 rec_len 合并
    if (ext2_remove_entry(dir, name) != 0) {
        inode_close(target_inode);
        return -EIO;
    }

    uint32_t now = (uint32_t)sys_time();

    // 更新引用计数
    // 目录被删除，父目录的 links_count 减 1 (对应子目录的 ..)
    dir->ext2_i.i_links_count--;

    dir->i_mtime = now;   // 内容变了
    dir->i_ctime = now;   // 链接数变了，ctime 必变

    sb->s_op->write_inode(dir);

    // 目标目录自身的 links_count 设为 0 以触发完全释放
    // 正常目录下 links_count 应该是 2 (父目录指向它，加上它自身的 .)
    target_inode->ext2_i.i_links_count = 0;

    target_inode->i_ctime = now;   // 链接数归零也是状态改变
    target_inode->ext2_i.i_dtime = now;   // 记录删除时间

    // 复用 truncate，里面会根据 i_size 来调整文件大小，此处需要将其置为 0
    target_inode->i_size = 0;
    // 彻底释放资源
    ext2_truncate(target_inode); // 释放所有数据块
    ext2_resource_free(sb, target_inode->i_no, EXT2_INODE_BITMAP); // 释放 Inode 位图

    // 同步并清理
    inode_evict(target_inode); // 从缓存抹除
    
    sb->s_op->write_super(sb);
    ext2_sync_gdt(sb);

    return 0;
}

// 在 dir 目录下查找名为 name 的条目，并返回其 inode 编号
// dir 父目录的 inode
// name 要查找的文件名
// len 文件名长度
// out_ino 用于存储找到的 inode 编号
// return 0 成功, -ENOENT 未找到
static int ext2_find_entry_ino(struct inode *dir, const char *name, int len, uint32_t *out_ino) {
    struct super_block *sb = dir->i_sb;
    uint32_t block_size = sb->s_block_size;
    void *io_buf = kmalloc(block_size);
    if (!io_buf) return -ENOMEM;

    // 计算逻辑块总数
    uint32_t total_blocks = DIV_ROUND_UP(dir->i_size, block_size);

    for (uint32_t i = 0; i < total_blocks; i++) {
        // 使用 bmap 找到物理块号
        uint32_t p_blk = ext2_bmap(dir, i);
        if (p_blk == 0) continue; // 目录空洞处理

        // 读取目录块
        partition_read(get_part_by_rdev(dir->i_dev), BLOCK_TO_SECTOR(sb, p_blk), io_buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry *de = (struct ext2_dir_entry *)io_buf;
        uint32_t offset = 0;

        // 在块内线性扫描
        while (offset < block_size) {
            // 检查是否为有效条目 (i_no != 0)
            // 检查文件名长度是否匹配 (防止前缀匹配)
            // 比较字符串内容
            if (de->i_no != 0 && de->name_len == len && memcmp(de->name, name, len) == 0) {
                *out_ino = de->i_no;
                kfree(io_buf);
                return 0; // 找到了
            }

            // 依据 rec_len 跳到下一个条目
            offset += de->rec_len;
            
            // 防止死循环或损坏的块
            if (de->rec_len == 0 || offset > block_size) break;
            
            de = (struct ext2_dir_entry *)((uint8_t *)de + de->rec_len);
        }
    }

    kfree(io_buf);
    return -ENOENT;
}

// 用于 rename 操作中检查环路
// 例如如果用户执行 mv /a /a/b/c，由于 /a 变成了 /a/b/c/a，目录树会断开并形成一个闭环。
// 我们需要让 new_dir 一路向 .. 回溯到根节点，看看其中的某一环会不会等于 old_inode
static bool is_ancestor(struct inode *old_inode, struct inode *new_dir) {
    uint32_t current_ino = new_dir->i_no;
    uint32_t root_ino = old_inode->i_sb->s_root_ino; // Ext2 根目录固定为 2
    struct inode *temp_inode = NULL;

    // 如果目标就是自己，直接拒绝
    if (current_ino == old_inode->i_no) return true;

    while (current_ino != root_ino) {
        // 打开当前的 ".." 目录
        temp_inode = inode_open(get_part_by_rdev(new_dir->i_dev), current_ino);

        if (!temp_inode) {
            printk("is_ancestor: fail to open inode %d\n", current_ino);
            return false; 
        }
        
        // 在当前目录中查找 ".." 的 Inode 编号
        uint32_t parent_ino = 0;
        if (ext2_find_entry_ino(temp_inode, "..", 2, &parent_ino) != 0) {
            inode_close(temp_inode);
            return false; // 理论上不该发生，说明目录损坏
        }
        
        if (parent_ino == old_inode->i_no) {
            inode_close(temp_inode);
            return true; // 找到了祖先，存在环路风险
        }
        
        current_ino = parent_ino;
        inode_close(temp_inode);
    }
    return false;
}

static void ext2_update_dotdot(struct inode *moved_dir, uint32_t new_parent_ino) {
    struct super_block *sb = moved_dir->i_sb;
    uint32_t block_size = sb->s_block_size;
    void *io_buf = kmalloc(block_size);
    
    // 目录的第一块（逻辑块 0）必然包含 "." 和 ".."
    // 因此第二个参数传 0 就行
    uint32_t p_blk = ext2_bmap(moved_dir, 0);
    partition_read(get_part_by_rdev(moved_dir->i_dev), BLOCK_TO_SECTOR(sb, p_blk), io_buf, block_size / SECTOR_SIZE);

    struct ext2_dir_entry *de = (struct ext2_dir_entry *)io_buf;
    // 第一个条目是 "."，跳过它
    de = (struct ext2_dir_entry *)((uint8_t *)de + de->rec_len);
    
    // 第二个条目必然是 ".."
    de->i_no = new_parent_ino;

    partition_write(get_part_by_rdev(moved_dir->i_dev), BLOCK_TO_SECTOR(sb, p_blk), io_buf, block_size / SECTOR_SIZE);
    kfree(io_buf);
}

// rename 操作无需检查目录是否为空，即使old_name所指向的目录不空，他也是能移动的
// 因为其下的各个目录项在移动的过程中并不会改变
static int ext2_rename(struct inode *old_dir, char *old_name, int old_len UNUSED, struct inode *new_dir, char *new_name, int new_len UNUSED) {

    // 如果是同名remove，那么直接返回，不做无用功
    if (old_dir == new_dir && strlen(old_name) == strlen(new_name) && 
        memcmp(old_name, new_name, strlen(old_name)) == 0) {
        return 0; 
    }

    struct super_block* sb = old_dir->i_sb;
    struct inode* old_inode = NULL;
    struct inode* new_inode = NULL;
    int32_t retval = -ENOENT;

    // 查找源文件 Inode
    if (ext2_lookup(old_dir, old_name, strlen(old_name), &old_inode) != 0) {
        return -ENOENT;
    }

    // 环路检测, 如果是目录移动，确保 new_dir 不是 old_inode 的子目录
    if (old_inode->i_type == FT_DIRECTORY) {
        if (is_ancestor(old_inode, new_dir)) {
            printk("ext2_rename: err! may create a loop!\n");
            retval = -EINVAL;
            goto out;
        }
    }

    // 检查目标是否已存在
    if (ext2_lookup(new_dir, new_name, strlen(new_name), &new_inode) == 0) {
        // 如果目标存在，且类型不匹配（一个文件一个目录），报错
        if (old_inode->i_type != new_inode->i_type) {
            retval = -ENOTDIR;
            goto out_new;
        }
        
        // 如果是目录，必须为空才能覆盖
        if (new_inode->i_type == FT_DIRECTORY && !ext2_is_dir_empty(new_inode)) {
            retval = -ENOTEMPTY;
            goto out_new;
        }

        // 先把目标位置的“坑”填平，也就是删除目标
        if (new_inode->i_type == FT_DIRECTORY) {
            ext2_rmdir(new_dir, new_name, strlen(new_name));
        } else {
            ext2_unlink(new_dir, new_name, strlen(new_name));
        }
        inode_close(new_inode);
        new_inode = NULL;
    }
    
    // 在新目录下添加条目
    if (ext2_add_entry(new_dir, old_inode->i_no, new_name, old_inode->i_type) != 0) {
        retval = -EIO;
        goto out;
    }

    // 从原位置抹除
    ext2_remove_entry(old_dir, old_name);

    uint32_t now = (uint32_t)sys_time();

    // 目标目录 (new_dir) 增加了条目
    new_dir->i_mtime = now;
    new_dir->i_ctime = now;

    // 源目录 (old_dir) 删除了条目
    old_dir->i_mtime = now;
    old_dir->i_ctime = now;

    // 被移动的文件/目录本身 (old_inode)
    // 即使只是改个名，它的状态（ctime）也算改变了
    old_inode->i_ctime = now;

    // 如果是目录移动，需要修正 ".." 
    // 这些修正操作最好放到 ext2_remove_entry 后面做
    // 不然若ext2_remove_entry在io过程中失败的话，会导致父目录的计数异常
    if (old_inode->i_type == FT_DIRECTORY) {
        if (old_dir != new_dir) {
            // 更新子目录内部的 ".." 指向新的父目录
            ext2_update_dotdot(old_inode, new_dir->i_no);
            
            // 维护父目录的链接计数
            new_dir->ext2_i.i_links_count++;
            old_dir->ext2_i.i_links_count--;
        }
    }

    sb->s_op->write_inode(new_dir);
    sb->s_op->write_inode(old_dir);
    sb->s_op->write_inode(old_inode);

    // 同步并返回
    sb->s_op->write_super(sb);
    ext2_sync_gdt(sb);
    retval = 0;

out_new:
    if (new_inode) inode_close(new_inode);
out:
    inode_close(old_inode);
    old_inode = NULL; 
    new_inode = NULL;
    return retval;
}

static int32_t ext2_readlink(struct inode* inode, char* buf, int bufsize) {
    uint32_t block_size = inode->i_sb->s_block_size;
    uint32_t path_len = inode->i_size;
    
    // 确保不会溢出用户提供的 buf
    if (path_len > (uint32_t)bufsize) {
        path_len = bufsize;
    }

    // 根据 i_blocks 是否为 0 判断是 Fast Symlink 还是 Normal Symlink
    // 普通符号链接中，如果目标路径（如 /usr/bin/python3）很长，内核会分配一个数据块，把路径存进去。
    // 快符号链接 (Fast Symlink)中，如果目标路径很短（小于 60 字节）
    // Ext2 会直接把路径字符串存放在 inode 结构体原本用来存放块地址的 i_block[15] 数组里。
    // 这么做的好处是不需要分配额外的数据块，节省空间，更重要的是减少了一次磁盘 IO。
    if (inode->i_blocks == 0) {
        // 快符号链接，路径直接存在 i_block 数组里
        memcpy(buf, (char*)inode->ext2_i.i_block, path_len);
        buf[path_len] = '\0';
        return path_len;
    }

    // 处理 Normal Symlink (路径存在外部磁盘块)
    struct partition* part = get_part_by_rdev(inode->i_dev);
    void* io_buf = kmalloc(block_size);
    
    // 符号链接的目标路径通常不会跨块（谁会写超过 1KB/4KB 的路径呢？况且我们系统限制的最长路径长度只有512字节）
    // 但为了严谨，我们还是按照逻辑块号来读。由于 i_size 记录了总长度，
    // 我们这里简单起见读出第一个块即可（对于大部分情况完全足够）。
    uint32_t phys_block = ext2_bmap(inode, 0);
    if (phys_block == 0) {
        kfree(io_buf);
        return -EIO; // 理论上不应出现
    }

    partition_read(part, BLOCK_TO_SECTOR(inode->i_sb, phys_block), io_buf, block_size / SECTOR_SIZE);
    
    // 将数据拷贝到输出缓冲区
    memcpy(buf, io_buf, path_len);
    buf[path_len] = '\0'; // 手动封死字符串 
    
    kfree(io_buf);
    return path_len;
}

// 创建一个符号链接
static int32_t ext2_symlink(struct inode* dir, char* name, int len UNUSED, const char* target) {
    struct super_block* sb = dir->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(sb->s_dev);
    
    uint32_t target_len = strlen(target);
    if (target_len >= sb->s_block_size) return -ENAMETOOLONG;

    // 分配 Inode 号
    uint32_t group = (dir->i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    int32_t inode_no = ext2_resource_alloc(sb, group, EXT2_INODE_BITMAP);
    if (inode_no == -1) return -ENOSPC;

    // 初始化内存中的 Inode
    struct inode* new_inode = (struct inode*)kmalloc(sizeof(struct inode));
    ext2_inode_init(part, inode_no, new_inode, FT_SYMLINK, 0777);
    
    uint32_t now = (uint32_t)sys_time();
    new_inode->i_atime = new_inode->i_mtime = new_inode->i_ctime = now;
    new_inode->i_size = target_len;

    int32_t phys_block = -1;

    // 区分 Fast 和 Normal
    if (target_len < FAST_LINK_LIMIT) {
        // 物理块数为 0，数据存入 i_block
        new_inode->i_blocks = 0;
        memcpy((char*)new_inode->ext2_i.i_block, target, target_len);
    } else {
        // 分配一个数据块存储路径
        phys_block = ext2_resource_alloc(sb, group, EXT2_BLOCK_BITMAP);
        if (phys_block == -1) {
            // 这里理想状态下要回滚 inode_bitmap，此处先暂略，直接panic
            PANIC("ext2_symlink: fail to ext2_resource_alloc");
            kfree(new_inode);
            return -ENOSPC;
        }
        new_inode->ext2_i.i_block[0] = phys_block;
        new_inode->i_blocks = sb->s_block_size / SECTOR_SIZE;

        // 将路径写入磁盘块
        void* io_buf = kmalloc(sb->s_block_size);
        memset(io_buf, 0, sb->s_block_size);
        memcpy(io_buf, target, target_len);
        partition_write(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, sb->s_block_size / SECTOR_SIZE);
        kfree(io_buf);
    }

    // 将该链接项添加到父目录
    if (ext2_add_entry(dir, inode_no, name, FT_SYMLINK) < 0) {
        ext2_resource_free(sb, inode_no, EXT2_INODE_BITMAP);
        if (target_len >= 60 && phys_block != -1) {
            ext2_resource_free(sb, phys_block, EXT2_BLOCK_BITMAP);
        }
        kfree(new_inode);
        return -EIO;
    }

    // 持久化并同步元数据
    sb->s_op->write_inode(new_inode);
    sb->s_op->write_inode(dir);
    ext2_sync_gdt(sb);
    sb->s_op->write_super(sb);

    kfree(new_inode);
    return 0;
}

// link 就是将不同的目录项指向同一个文件
// 符号链接可以跨设备，但是硬链接不可以
// 因为符号链接的唯一标记其实就是一个路径，所以不管跨不跨设备，他只把握这个路径就行了，因此它可以跨设备
// 但是硬链接它的标记是它指向的 inode，而这个 inode 的 inode 索引在不同分区上可能相同
// 这导致硬链接如果可以跨分区的话会出现指代模糊的情况

// 文件系统底层不检查文件名是否重复，文件名是否重复是交给vfs层检查的
static int32_t ext2_link(struct inode* old_inode, struct inode* dir, const char* name, int len UNUSED) {
    // 硬链接不能指向目录（Linux 的标准做法，防止文件系统拓扑变成图而不是树）
    if (old_inode->i_type == FT_DIRECTORY) {
        return -EPERM; 
    }

    // 检查是否跨设备（VFS 层通常会处理，但在底层驱动做一次校验更安全）
    if (old_inode->i_dev != dir->i_dev) {
        return -EXDEV;
    }

    // 增加源文件的硬链接计数
    old_inode->ext2_i.i_links_count++;
    
    // 更新 ctime (Inode 状态改变)
    uint32_t now = (uint32_t)sys_time();
    old_inode->i_ctime = now;

    // 在目标目录中添加新的目录项
    // 注意，file_type 应该和源文件保持一致
    int32_t ret = ext2_add_entry(dir, old_inode->i_no, (char*)name, old_inode->i_type);
    if (ret < 0) {
        // 如果添加失败（如磁盘空间不足），需要回滚计数
        old_inode->ext2_i.i_links_count--;
        return ret;
    }

    // 更新目标目录的时间戳
    dir->i_mtime = now;
    dir->i_ctime = now;

    // 持久化到磁盘
    struct super_block* sb = old_inode->i_sb;
    sb->s_op->write_inode(old_inode); // 更新源文件 Inode (links_count)
    sb->s_op->write_inode(dir);       // 更新目录 Inode (mtime/ctime)

    return 0;
}


struct inode_operations ext2_file_inode_operations = {
    .default_file_ops = &ext2_file_file_operations,
    .create     = NULL,
    .lookup     = NULL,
    .unlink     = NULL, // unlink 涉及到修改目录项，只能放到dir的inode操作里
    .mkdir      = NULL,
    .rmdir      = NULL,
    .mknod      = NULL,
    .rename     = NULL, // rename 涉及到修改目录项，只能放到dir的inode操作里
    .bmap       = ext2_bmap, 
    .truncate   = ext2_truncate,
    .symlink    = NULL, 
    .readlink   = NULL,
    .link       = NULL,
};

struct inode_operations ext2_dir_inode_operations = {
    .default_file_ops = &ext2_dir_file_operations,
    .create     = ext2_create,
    .lookup     = ext2_lookup,
    .unlink     = ext2_unlink,
    .mkdir      = ext2_mkdir,
    .rmdir      = ext2_rmdir,
    .mknod      = ext2_mknod,
    .rename     = ext2_rename,
    .bmap       = ext2_bmap, 
    .truncate   = NULL, // 目录不能 truncate
    .symlink    = ext2_symlink, // ext2_symlink 本质上也是一个创建操作，和mknod一样，给目录操作集
    .readlink   = NULL,
    .link       = ext2_link,
};

struct inode_operations ext2_chardev_inode_operations = {
    .default_file_ops = NULL,
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

struct inode_operations ext2_blkdev_inode_operations = {
    .default_file_ops = NULL,
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

struct inode_operations ext2_symlink_inode_operations = {
    .default_file_ops = NULL,
    // 符号链接通常不允许在该目录下创建东西，所以其他函数为 NULL
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
    .readlink = ext2_readlink,
    .link       = NULL,
};