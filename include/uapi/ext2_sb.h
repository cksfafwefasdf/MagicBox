#ifndef __INCLUDE_UAPI_EXT2_SB_H
#define __INCLUDE_UAPI_EXT2_SB_H
#include <ext2_fs.h>
#include <stdint.h>

struct partition;
struct super_block;

enum ext2_bitmap_type {
	EXT2_INODE_BITMAP,
	EXT2_BLOCK_BITMAP
};

struct ext2_group_desc {
    uint32_t bg_block_bitmap; // 块位图所在的块号
    uint32_t bg_inode_bitmap; // Inode 位图所在的块号
    uint32_t bg_inode_table; // Inode 表起始块号
    uint16_t bg_free_blocks_count; // 本块组空闲块数
    uint16_t bg_free_inodes_count; // 本块组空闲 Inode 数
    uint16_t bg_used_dirs_count; // 本块组目录数
    uint16_t bg_pad; // 填充，对齐到 2 字节
    uint32_t bg_reserved[3]; // 保留字段，凑齐 32 字节
} __attribute__((packed));

struct ext2_sb_info {
    struct ext2_super_block sb_raw;    // 原始磁盘超级块 (1024B)
    
    // 运行时计算的辅助字段
    // 为了节省空间，ext2中block_size并不是直接存的块大小，而是存的2的n次幂的n
    // 例如 1024 存的是 1024 * 2^0 中的 0
    // 2048 存的是 1024 * 2^1 中的 1，依次类推，这样我们用位移操作就能很快得到块的大小，十分方便
    uint32_t block_size; // 1024 << s_log_block_size
    uint32_t group_desc_cnt; // 总共有多少个块组
    
    // 块组描述符表 (GDT) 缓存
    // 它是定位所有位图和 Inode Table 的“地图”
    struct ext2_group_desc* group_desc; 

    // Ext2 的位图是分块组的。
    // 极简实现下，如果只用 20MB 的盘，通常可以只用一个块组。
    // 如果要更通用，相关的位图管理可能需要按需加载。
    // 暂时我们可以只缓存第一个块组的位图，或者先不缓存，读的时候现场读。
};

extern void ext2_sync_gdt(struct super_block *sb);
extern uint16_t ext2_encode_type(enum file_types ft,uint16_t mode);
extern enum file_types ext2_decode_type(uint16_t mode);

extern struct file_system_type ext2_fs_type;
extern struct super_operations ext2_super_ops;
#endif