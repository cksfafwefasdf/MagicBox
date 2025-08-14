#ifndef __FS_SUPER_BLOCK_H
#define __FS_SUPER_BLOCK_H
#include "../lib/stdint.h"

struct super_block{
	uint32_t magic; // the type of the file system
	uint32_t sec_cnt; // the number of the sector in this partition
	uint32_t inode_cnt; // the number of the inode in this partition 
	uint32_t part_lba_base; // the start LBA of this partition
	
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


#endif