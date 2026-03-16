#ifndef __INCLUDE_UAPI_EXT2_INODE_H
#define __INCLUDE_UAPI_EXT2_INODE_H

#include <stdint.h>

struct partition;

struct ext2_inode_info {
    /* 核心指针：12直接, 1一级间接, 1二级间接, 1三级间接 */
    uint32_t i_block[15]; 
    
    // 存储 Ext2 原始的 i_mode (包含 Linux 标准权限和类型) 
    // 虽然 VFS inode 有 i_type，但写回磁盘时需要原始的权限位
    uint16_t i_mode;
    
    // 硬链接计数
    // 由于 i_mode 是 16 位的，为了凑齐 32 位对齐，我们再把 i_links_count 也加进来
    // 这样也便于之后实现硬链接
    uint16_t i_links_count;

    // 占用块数（512B 扇区为单位）
    uint32_t i_blocks;

    uint32_t i_size;

    // 对于非常短的符号链接（小于 60 字节），Ext2 会直接把路径存进 i_block 指针数组里
    // 而不是去分配一个新的数据块。这叫 fast symlink。
};

extern void ext2_lookup_test(struct partition* part);

extern struct inode_operations ext2_inode_operations;

#endif