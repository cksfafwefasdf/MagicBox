#ifndef __INCLUDE_UAPI_SIFS_FS_H
#define __INCLUDE_UAPI_SIFS_FS_H

#include <stdint.h>
#include <unitype.h>
#include <sifs_inode.h>

/*
    该文件中定义的全是 sifs 对应的磁盘镜像
*/

#define SIFS_FS_MAGIC_NUMBER 0x20030000 // magic number for this file system

#define SIFS_BLOCK_SIZE SECTOR_SIZE

struct sifs_super_block{
	uint32_t magic; // the type of the file system
	uint32_t sec_cnt; // the number of the sector in this partition
	uint32_t inode_cnt; // the number of the inode in this partition 
	
	// 文件系统的lba都是相对lba
	// 文件系统不管理绝对lba，绝对lba是由ide驱动来负责转换的
	// 具体来讲，这个相对lba转绝对lba的操作是在 和 partition_read partition_write 这两个宏里面进行的
	// 因此文件系统只能用这两个宏
	// uint32_t part_lba_base; // the start LBA of this partition
	
	// 此处的lba都是相对lba，从0开始
	uint32_t block_bitmap_lba;
	uint32_t block_bitmap_sects;
	
	uint32_t inode_bitmap_lba;
	uint32_t inode_bitmap_sects;

	uint32_t inode_table_lba;
	uint32_t inode_table_sects;

	uint32_t data_start_lba; // the start of the data setion in this partition
	uint32_t root_inode_no; // the inode number of the root dict
	uint32_t dir_entry_size; // size of each dict entry

	// make the size of the superblock be 512B (1 sector)
	uint8_t pad[460];

}__attribute__((packed));

// 针对不同文件系统，存储在磁盘上的inode实体
struct sifs_inode{
	enum file_types i_type;
	// when inode points to file, i_size is the size of the file
	// when inode points to dict file, i_size is the size of the sum of the dict entry
	uint32_t i_size;
	struct sifs_inode_info sii;
};


// 我们现在取消了 dir 结构体，我们将 dir 结构体并入到 file 结构体中
// 将 dir 也看做一种文件，然后通过文件类型来分发操作
// 这是同步会磁盘时，针对不同的文件系统所设置的不同磁盘镜像结构体
struct sifs_dir_entry {
    char filename[MAX_FILE_NAME_LEN];
    uint32_t i_no;
    enum file_types f_type;
};

#endif