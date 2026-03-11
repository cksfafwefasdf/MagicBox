#ifndef __INCLUDE_UAPI_SIFS_SB_H
#define __INCLUDE_UAPI_SIFS_SB_H

#include <sifs_fs.h>
#include <bitmap.h>

struct super_block;
struct statfs;

enum bitmap_type {
	INODE_BITMAP,
	BLOCK_BITMAP
};

// VFS 针对不同文件系统的超级块打的补丁
struct sifs_sb_info{
    struct sifs_super_block sb_raw;
    struct bitmap block_bitmap; // 属于 SIFS 的私有管理工具
    struct bitmap inode_bitmap;
};

struct inode_position{
	bool two_sec; // whether an inode spans multiple sectors
	uint32_t sec_lba;
	uint32_t off_size; // byte offset of the inode within the sectors
};

extern void bitmap_sync(struct partition* part,uint32_t bit_idx,enum bitmap_type btmp_type);
extern int32_t block_bitmap_alloc(struct partition* part);
extern int32_t inode_bitmap_alloc(struct partition* part);

extern void sifs_format(struct partition* part);
extern void sifs_inode_delete(struct partition* part,uint32_t inode_no,void* io_buf);

extern struct file_system_type sifs_fs_type;
extern struct super_operations sifs_super_ops;
#endif