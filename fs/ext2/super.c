#include <ext2_sb.h>
#include <memory.h>
#include <ide.h>
#include <stdio-kernel.h>
#include <ide.h>
#include <debug.h>
#include <fs_types.h>
#include <inode.h>
#include <unistd.h>

// 将 ext2 的 i_mode 字段转换成我们系统的 i_type 字段 
static enum file_types ext2_decode_type(uint16_t mode) {
    if (S_ISREG(mode))  return FT_REGULAR;
    if (S_ISDIR(mode))  return FT_DIRECTORY;
    if (S_ISCHR(mode))  return FT_CHAR_SPECIAL;
    if (S_ISBLK(mode))  return FT_BLOCK_SPECIAL;
    if (S_ISFIFO(mode)) return FT_FIFO;
    // if (S_ISLNK(mode))  return FT_SYMLINK;
    // if (S_ISSOCK(mode)) return FT_SOCKET;
    return FT_UNKNOWN;
}

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
    if (raw->s_magic != 0xEF53) {
        if (!silent) printk("VFS: device %d is not an ext2 filesystem\n", sb->s_dev);
        return NULL;
    }

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

    // 读取磁盘数据
    char* inode_buf = (char*)kmalloc(SECTOR_SIZE);

    ASSERT(inode_buf != NULL);

    partition_read(part, sec_lba, inode_buf, 1);
    
    struct ext2_inode* ei = (struct ext2_inode*)(inode_buf + off_in_sec);

    // 填充 VFS 通用字段 
    inode->i_size = ei->i_size;
    inode->i_type = ext2_decode_type(ei->i_mode);

    // 填充 Ext2 私有字段 
    memcpy(inode->ext2_i.i_block, ei->i_block, sizeof(uint32_t) * 15);
    inode->ext2_i.i_mode = ei->i_mode;
    inode->ext2_i.i_links_count = ei->i_links_count;
    inode->ext2_i.i_blocks = ei->i_blocks;
    inode->ext2_i.i_size = ei->i_size; // 记录原始大小

    // 处理特殊设备文件
    if (inode->i_type == FT_CHAR_SPECIAL || inode->i_type == FT_BLOCK_SPECIAL) {
        inode->i_rdev = ei->i_block[0]; 
    }

    // 绑定操作集，先默认全部绑定在ext2_inode_operations上，之后再来分
    switch (inode->i_type) {
        case FT_REGULAR:
            inode->i_op = &ext2_inode_operations;
            break;
        case FT_DIRECTORY:
            inode->i_op = &ext2_inode_operations;
            break;
        case FT_CHAR_SPECIAL:
        case FT_BLOCK_SPECIAL:
            inode->i_op = &ext2_inode_operations;
            break;
        default:
            inode->i_op = NULL;
            break;
    }

    kfree(inode_buf);
}

static void ext2_put_super(struct super_block *sb) {
    if (sb->ext2_info.group_desc) {
        kfree(sb->ext2_info.group_desc);
        sb->ext2_info.group_desc = NULL;
    }
    // 后期的位图啥的操作也要在这里完成，目前只实现了块组描述符的加载，所以先释放块组描述符
}

struct super_operations ext2_super_ops = {
    .read_inode  = ext2_read_inode,
    .write_inode = NULL, 
    .put_inode   = NULL,
    .put_super   = ext2_put_super,
    .write_super = NULL,
    .statfs      = NULL 
};

struct file_system_type ext2_fs_type = {
    .name = "ext2",
    .flags = FS_REQUIRES_DEV, // sifs是有设备对应的，不是内存文件系统
    .read_super = ext2_read_super 
};