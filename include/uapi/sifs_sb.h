#ifndef __INCLUDE_UAPI_SIFS_SB_H
#define __INCLUDE_UAPI_SIFS_SB_H

#include <sifs_fs.h>
#include <bitmap.h>

struct super_block;

// VFS 针对不同文件系统的超级块打的补丁
struct sifs_sb_info{
    struct sifs_super_block sb_raw;
    struct bitmap block_bitmap; // 属于 SIFS 的私有管理工具
    struct bitmap inode_bitmap;
};

extern void sifs_format(struct partition* part);
extern struct super_block * sifs_read_super(struct super_block *sb, void *data, int silent);

#endif