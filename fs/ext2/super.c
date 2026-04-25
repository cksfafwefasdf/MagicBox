#include <ext2_sb.h>
#include <memory.h>
#include <ide.h>
#include <stdio-kernel.h>
#include <ide.h>
#include <debug.h>
#include <fs_types.h>
#include <inode.h>
#include <unitype.h>
#include <time.h>

static struct super_block * ext2_read_super(struct super_block *sb, void *data UNUSED, int silent) {

    // 获取物理分区
    struct partition* part = get_part_by_rdev(sb->s_dev);
    if (part == NULL) {
        if (!silent) printk("VFS: can't find partition for dev %d\n", sb->s_dev);
        return NULL;
    }
    part->sb = sb;

    // 读取原始超级块 (偏移 1024 字节 = 2 扇区)
    // Ext2 超级块始终占用 1024 字节，即使结构体没定义完
    // 因此此处我们全部写死成2扇区
    partition_read(part, 2, &sb->ext2_info.sb_raw, 2);

    struct ext2_super_block* raw = &sb->ext2_info.sb_raw;

    // 校验魔数
    if (raw->s_magic != EXT2_MAGIC_NUMBER) {
        if (!silent) printk("VFS: device %d is not an ext2 filesystem\n", sb->s_dev);
        return NULL;
    }

    // 更新挂载时间 
    // raw->s_mtime 是 uint32_t，所以这里会截断，但符合 Ext2 磁盘规范
    raw->s_mtime = (uint32_t)sys_time();

    // 增加挂载计数
    raw->s_mnt_count++;

    //计算运行时辅助字段
    // BlockSize = 1024 << s_log_block_size
    sb->ext2_info.block_size = EXT2_BLOCK_UNIT << raw->s_log_block_size;
    
    // 计算总共有多少个块组
    // 总块数 / 每组块数，向上取整
    sb->ext2_info.group_desc_cnt = DIV_ROUND_UP(raw->s_blocks_count, raw->s_blocks_per_group);

    // 同步 VFS 通用字段
    sb->s_block_size = sb->ext2_info.block_size;
    sb->s_magic = raw->s_magic;
    sb->s_root_ino = 2; // Ext2 根目录固定为 Inode 2

    // 缓存块组描述符表 (GDT)
    // 计算 GDT 总字节大小
    uint32_t gdt_size = sb->ext2_info.group_desc_cnt * sizeof(struct ext2_group_desc);

    // 对于 1KB 块，GDT 在 Block 2；对于 >1KB 块，GDT 在 Block 1
    uint32_t gdt_block = (sb->s_block_size == EXT2_BLOCK_UNIT) ? 2 : 1;
    // 定位 GDT 的 LBA
    // 在 1KB 块大小下，SB 在 Block 1，GDT 在 Block 2 (即 LBA 4)
    // 通用公式：(s_first_data_block + 1) * (block_size / 512)
    uint32_t gdt_lba = BLOCK_TO_SECTOR(sb, gdt_block);
    uint32_t gdt_sects = DIV_ROUND_UP(gdt_size, SECTOR_SIZE);

    sb->ext2_info.group_desc = (struct ext2_group_desc*)kmalloc(gdt_sects*SECTOR_SIZE);

    

    // 在ext2中，块组描述符有可能在其他的块组内部还会单独有一个备份
    // 但是无论如何，我们只去读分区最开始的那个一定存在的原本就行
    partition_read(part, gdt_lba, sb->ext2_info.group_desc, gdt_sects);

    // 挂载操作集
    sb->s_op = &ext2_super_ops;

    // 建立根目录的 inode
    sb->s_root_inode = inode_open(part, sb->s_root_ino);
    
    if (sb->s_root_inode == NULL) {
        kfree(sb->ext2_info.group_desc);
        PANIC("ext2_read_super: fail to get root inode");
        return NULL;
    }

    if (!silent) {
        printk("VFS: ext2 mounted on dev %x (block_size: %x, groups: %x)\n", 
                sb->s_dev, sb->s_block_size, sb->ext2_info.group_desc_cnt);
    }

    return sb;
}

static void ext2_read_inode(struct inode* inode) {

    ASSERT(inode->i_no > 0); // Ext2 Inode 必须从 1 开始

    struct super_block* sb = inode->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(inode->i_dev);

    // 寻址定位 
    // Ext2 Inode 编号从 1 开始，而不是和sifs一样从0开始
    uint32_t i_no = inode->i_no;
    uint32_t group = (i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    uint32_t index = (i_no - 1) % ext2_info->sb_raw.s_inodes_per_group;

    // 找到该组 Inode Table 起始块号
    uint32_t inode_table_block = ext2_info->group_desc[group].bg_inode_table;
    
    // 计算字节偏移
    // 标准 Ext2 Inode 大小一般为128字节
    // 我们现在定义的 ext2_inode 确实也是 128 字节
    // 其实此处更标准的做法应该是从超级块里面读这个字段，但是此处为了简单，就先硬编码了
    uint32_t inode_size = sizeof(struct ext2_inode); 
    uint32_t byte_offset = index * inode_size;
    
    // 转换为扇区偏移
    uint32_t sec_lba = BLOCK_TO_SECTOR(sb, inode_table_block) + (byte_offset / SECTOR_SIZE);
    uint32_t off_in_sec = byte_offset % SECTOR_SIZE;

    struct buffer_head* bh = bread(part, sec_lba);
    char* inode_buf = (char*) bh->b_data;

    // 读取磁盘数据
    // char* inode_buf = (char*)kmalloc(SECTOR_SIZE);

    ASSERT(inode_buf != NULL);

    // partition_read(part, sec_lba, inode_buf, 1);
    
    
    struct ext2_inode* ei = (struct ext2_inode*)(inode_buf + off_in_sec);

    // 填充 VFS 通用字段 
    inode->i_size = ei->i_size;
    inode->i_type = decode_imode(ei->i_mode);
    inode->i_mode = ei->i_mode;
    inode->i_nlink = ei->i_links_count;

    // 填充 Ext2 私有字段 
    memcpy(inode->ext2_i.i_block, ei->i_block, sizeof(uint32_t) * 15);
    inode->ext2_i.i_links_count = ei->i_links_count;
    inode->i_blocks = ei->i_blocks;

    inode->i_atime = ei->i_atime;
    inode->i_mtime = ei->i_mtime;
    inode->i_ctime = ei->i_ctime;
    inode->ext2_i.i_dtime = ei->i_dtime;

    // 处理特殊设备文件
    if (inode->i_type == FT_CHAR_SPECIAL || inode->i_type == FT_BLOCK_SPECIAL) {
        inode->i_rdev = ei->i_block[0]; 
    }

    switch (inode->i_type) {
        case FT_REGULAR:
            inode->i_op = &ext2_file_inode_operations;
            break;
        case FT_DIRECTORY:
            inode->i_op = &ext2_dir_inode_operations;
            break;
        case FT_CHAR_SPECIAL:
            inode->i_op = &ext2_chardev_inode_operations;
            break;
        case FT_BLOCK_SPECIAL:
            inode->i_op = &ext2_blkdev_inode_operations;
            break;
        case FT_SYMLINK:
            inode->i_op = &ext2_symlink_inode_operations;
            break;
        default:
            inode->i_op = NULL;
            break;
    }

    // kfree(inode_buf);
    brelse(bh);
}

static void ext2_write_inode(struct inode* inode) {
    struct super_block* sb = inode->i_sb;
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct partition* part = get_part_by_rdev(inode->i_dev);

    // 定位该 Inode 在磁盘上的位置
    uint32_t i_no = inode->i_no;
    // Ext2 Inode 编号从 1 开始计算
    uint32_t group = (i_no - 1) / ext2_info->sb_raw.s_inodes_per_group;
    uint32_t index = (i_no - 1) % ext2_info->sb_raw.s_inodes_per_group;

    // 找到对应块组的 Inode Table 起始块号
    uint32_t inode_table_block = ext2_info->group_desc[group].bg_inode_table;
    
    // 计算在 Inode Table 内部的字节偏移
    uint32_t inode_size = sizeof(struct ext2_inode);
    uint32_t byte_offset = index * inode_size;
    
    // 转换为扇区 LBA 和扇区内偏移
    uint32_t sec_lba = BLOCK_TO_SECTOR(sb, inode_table_block) + (byte_offset / SECTOR_SIZE);
    uint32_t off_in_sec = byte_offset % SECTOR_SIZE;

    struct buffer_head* bh = bread(part, sec_lba);
    char* io_buf = (char*) bh->b_data;

    // Read-Modify-Write (RMW) 过程
    // 即使 Ext2 不会跨扇区（因为 ext2 的 inode 的大小是128或者256，不会跨扇区）
    // 但为了安全起见我们仍分配 1 个扇区缓冲区
    // char* io_buf = (char*)kmalloc(SECTOR_SIZE);
    // if (io_buf == NULL) {
    //     PANIC("ext2_write_inode: fail to kmalloc for io_buf");
    // }

    // 先读出整块扇区，避免破坏同扇区的其他 Inode
    // partition_read(part, sec_lba, io_buf, 1);

    // 找到缓冲区中的目标位置
    struct ext2_inode* ei = (struct ext2_inode*)(io_buf + off_in_sec);

    // 同步字段
    ei->i_mode = inode->i_mode; // 包含文件类型和权限
    ei->i_size = inode->i_size;
    ei->i_links_count = inode->ext2_i.i_links_count;
    ei->i_blocks = inode->i_blocks; // 这是以 512 字节为单位的计数，而不是真的按 block 计数
    
    // 拷贝块寻址数组 (i_block[15])
    memcpy(ei->i_block, inode->ext2_i.i_block, sizeof(uint32_t) * 15);

    // 此处 now 的更新不能使用 update_time 函数！
    // 因为 update_time 的底层又调用了 ext2_write_inode，如果我们直接调用 update_time
    // 那么就会出现循环调用了！
    uint32_t now = (uint32_t)sys_time();

    // 更新 ctime (Status Change Time)
    // 只要进入了这个 write_inode 函数，说明 inode 的元数据（大小、链接数、块指向）变了
    // 按照 Unix 标准，此时必须更新 ctime。
    inode->i_ctime = now;

    // 将内存中的三个时间戳刷入磁盘镜像 (ei 是磁盘上的结构)
    ei->i_atime = inode->i_atime;
    ei->i_mtime = inode->i_mtime;
    ei->i_ctime = inode->i_ctime;

    // 处理 dtime (Deletion Time)
    // 如果链接数为 0，说明文件被删除了，记录删除时间
    if (inode->ext2_i.i_links_count == 0) {
        ei->i_dtime = now;
    } else {
        ei->i_dtime = 0;
    }

    // 写回磁盘
    // partition_write(part, sec_lba, io_buf, 1);
    bwrite(bh);

    // kfree(io_buf);
    brelse(bh);
}

// 由于 ext2 的超级块里面维护了很多实时的计数信息，因此如果要卸载超级块的话必须要同步一下
// 该函数只负责同步超级块，其余的像是块组描述符、位图啥的不在此同步，由 mkdir create 等操作自行调用，这样也更灵活
static void ext2_write_super(struct super_block *sb) {
    struct partition *part = get_part_by_rdev(sb->s_dev);
    
    sb->ext2_info.sb_raw.s_wtime = (uint32_t)sys_time();

    // Ext2 超级块始终在分区的 1024 字节偏移处
    // 无论块大小是 1KB, 2KB 还是 4KB，LBA 始终是从 2 开始（512字节扇区）
    // 超级块结构体本身 1024 字节，所以占 2 个扇区
    uint32_t sb_lba = 2; 
    uint32_t sb_sects = 2;

    // 直接将内存中维护的 sb_raw 写回磁盘
    partition_write(part, sb_lba, &sb->ext2_info.sb_raw, sb_sects);
}

static void ext2_put_super(struct super_block *sb) {
    if (sb->ext2_info.group_desc) {
        kfree(sb->ext2_info.group_desc);
        sb->ext2_info.group_desc = NULL;
    }
    // 后期的位图啥的操作也要在这里完成，目前只实现了块组描述符的加载，所以先释放块组描述符
}

static void ext2_statfs(struct super_block *sb, struct statfs *buf) {
    struct ext2_sb_info* ext2_info = &sb->ext2_info;
    struct ext2_super_block* raw_sb = &ext2_info->sb_raw;

    // 填充文件系统基本信息
    buf->f_type = raw_sb->s_magic;// Ext2 Magic: 0xEF53
    buf->f_bsize = sb->s_block_size; // 块大小 (1KB, 2KB 或 4KB)
    buf->f_blocks = raw_sb->s_blocks_count; // 总块数（扇区数，不是真的块数）
    buf->f_namelen = EXT2_MAX_FILE_NAME_LEN; // Ext2 默认最大文件名长度

    // 直接从超级块读取空闲信息 (账本数据)
    // s_free_blocks_count 是全局总空闲块
    buf->f_bfree = raw_sb->s_free_blocks_count;
    
    // Ext2 有为超级用户保留块的概念 (s_r_blocks_count)
    // 普通用户可用的空间 = 总空闲 - 保留空间
    if (buf->f_bfree > raw_sb->s_r_blocks_count) {
        buf->f_bavail = buf->f_bfree - raw_sb->s_r_blocks_count;
    } else {
        buf->f_bavail = 0;
    }

    // 填充 Inode 相关信息
    buf->f_files = raw_sb->s_inodes_count; // 总 Inode 数
    buf->f_ffree = raw_sb->s_free_inodes_count; // 空闲 Inode 数
}

//  块组描述符同步函数
void ext2_sync_gdt(struct super_block *sb) {
    struct ext2_sb_info *ext2_info = &sb->ext2_info;
    struct partition *part = get_part_by_rdev(sb->s_dev);

    uint32_t gdt_block = (sb->s_block_size == 1024) ? 2 : 1;
    uint32_t gdt_lba = BLOCK_TO_SECTOR(sb, gdt_block);
    
    // 整个 GDT 数组同步写回
    uint32_t gdt_size = ext2_info->group_desc_cnt * sizeof(struct ext2_group_desc);
    partition_write(part, gdt_lba, ext2_info->group_desc, DIV_ROUND_UP(gdt_size, SECTOR_SIZE));
}

struct super_operations ext2_super_ops = {
    .read_inode  = ext2_read_inode,
    .write_inode = ext2_write_inode, 
    .put_inode   = NULL, // 我们使用直写式缓存，不做延迟写，所以此处为NULL
    .put_super   = ext2_put_super,
    .write_super = ext2_write_super,
    .statfs      = ext2_statfs 
};

struct file_system_type ext2_fs_type = {
    .name = "ext2",
    .flags = FS_REQUIRES_DEV, // sifs是有设备对应的，不是内存文件系统
    .read_super = ext2_read_super 
};