#include <fs_types.h>
#include <ide_buffer.h>
#include <fs.h>
#include <ide.h>
#include <debug.h>
#include <global.h>
#include <stdio-kernel.h>
#include <inode.h>

// logic formatlize 
void sifs_format(struct partition* part){
	uint32_t boot_sector_sects = 1;
	uint32_t super_block_sects = 1;
	uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART,BITS_PER_SECTOR);
	uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct sifs_inode)*MAX_FILES_PER_PART)),SECTOR_SIZE);
	uint32_t used_sects = boot_sector_sects+super_block_sects+inode_bitmap_sects+inode_table_sects;
	uint32_t free_sects = part->sec_cnt - used_sects;

	uint32_t block_bitmap_sects;
	block_bitmap_sects = DIV_ROUND_UP(free_sects,BITS_PER_SECTOR);
	uint32_t block_bitmap_bit_len = free_sects-block_bitmap_sects;
	block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len,BITS_PER_SECTOR);

	struct sifs_super_block sb;
	sb.magic = SIFS_FS_MAGIC_NUMBER;
	sb.sec_cnt = part->sec_cnt;
	sb.inode_cnt = MAX_FILES_PER_PART;
	// sb.part_lba_base = part->start_lba;

	sb.block_bitmap_lba = 2;
	sb.block_bitmap_sects = block_bitmap_sects;

	sb.inode_bitmap_lba = sb.block_bitmap_lba+sb.block_bitmap_sects;
	sb.inode_bitmap_sects = inode_bitmap_sects;

	sb.inode_table_lba = sb.inode_bitmap_lba+sb.inode_bitmap_sects;
	sb.inode_table_sects = inode_table_sects;

	sb.data_start_lba = sb.inode_table_lba+sb.inode_table_sects;
	sb.root_inode_no = 0;
	sb.dir_entry_size = sizeof(struct sifs_dir_entry);

	printk("%s info:\n",part->name);
	printk("\tmagic:0x%x\n\
\tpart_lba_base:0x%x\n\
\tall_sectors:0x%x\n\
\tinode_cnt:0x%x\n\
\tblock_bitmap_lba:0x%x\n\
\tblock_bitmap_sectors:0x%x\n\
\tinode_bitmap_lba:0x%x\n\
\tinode_bitmap_sectors:0x%x\n\
\tinode_table_lba:0x%x\n\
\tinode_table_sectors:0x%x\n\
\tdata_start_lba:0x%x\n", 
	sb.magic,
	part->start_lba,
	sb.sec_cnt,
	sb.inode_cnt,
	sb.block_bitmap_lba,sb.block_bitmap_sects,
	sb.inode_bitmap_lba,sb.inode_bitmap_sects,
	sb.inode_table_lba,sb.inode_table_sects,
	sb.data_start_lba);

	// struct disk* hd = part->my_disk; 

	// write superblock to the no.1 sector (no.0 is obr)
	partition_write(part,1,&sb,1);
	printk("\tsuper_block_lba:0x%x\n",part->start_lba+1);
	
	// find the biggest meta info
	// use it's size as buf_size
	uint32_t buf_size = sb.block_bitmap_sects>=sb.inode_bitmap_sects?sb.block_bitmap_sects:sb.inode_bitmap_sects;
	buf_size = (buf_size>=sb.inode_table_sects?buf_size:sb.inode_table_sects)*SECTOR_SIZE;
	
	uint8_t* buf = (uint8_t*)kmalloc(buf_size);

	// init block_bitmap and write it to sb.block_bitmap_lba
	buf[0] |= 0x01; // 0th block is used for root dict, reserve it
	uint32_t block_bitmap_last_byte = block_bitmap_bit_len/8;
	uint32_t block_bitmap_last_bit = block_bitmap_bit_len%8;
	uint32_t last_size = SECTOR_SIZE-(block_bitmap_last_byte%SECTOR_SIZE);

	memset(&buf[block_bitmap_last_byte],0xff,last_size);

	uint8_t bit_idx = 0;
	while(bit_idx<=block_bitmap_last_bit){
		buf[block_bitmap_last_byte] &= ~(1<<bit_idx++);
	}
	partition_write(part,sb.block_bitmap_lba,buf,sb.block_bitmap_sects);


	// init inode_bitmap and write it to sb.inode_bitmap_lba
	// flush the buf
	memset(buf,0,buf_size);
	buf[0] |= 0x1; // reserve for root dict
	partition_write(part,sb.inode_bitmap_lba,buf,sb.inode_bitmap_sects);

	// init inode list and write it to sb.inode_table_lba
	// flush the buf
	memset(buf,0,buf_size);
	struct sifs_inode* i = (struct sifs_inode*)buf;
	i->i_size = sb.dir_entry_size*2; // dict '.' and '..'
	// i->i_no = 0; // 0th is used for root dict 
	i->sii.i_sectors[0] = sb.data_start_lba;
	i->i_type = FT_DIRECTORY;

	partition_write(part,sb.inode_table_lba,buf,sb.inode_table_sects);

	// write root dict to sb.data_start_lba
	// write dict '.' and '..'
	memset(buf,0,buf_size);
	struct sifs_dir_entry* p_de = (struct sifs_dir_entry*)buf;

	// init current dict '.'
	memcpy(p_de->filename,".",1);
	p_de->i_no = 0;
	p_de->f_type = FT_DIRECTORY;
	p_de++;

	// init parent dict ".."
	memcpy(p_de->filename,"..",2);
	p_de->i_no = 0;
	p_de->f_type = FT_DIRECTORY;

	// sb.data_start_lba has been allocated to the root dict which contains dict entries
	partition_write(part,sb.data_start_lba,buf,1);
	
	kfree(buf);
	printk("\troot_dir_lba:0x%x\n",sb.data_start_lba);
	printk("%s format done\n",part->name);
}

struct super_block * sifs_read_super(struct super_block *sb, void *data, int silent) {
    // 通过逻辑设备号（s_dev）定位到分区结构体
    struct partition* part = get_part_by_rdev(sb->s_dev);
    
    if (part == NULL) {
        if (!silent) printk("VFS: can't find partition for dev %d\n", sb->s_dev);
        return NULL;
    }

    part->sb = sb; // 确保part持有sb

    // 读取超级块 (在分区起始 LBA + 1)
    partition_read(part,1, &sb->sifs_info.sb_raw, 1);

    struct sifs_super_block* raw = &sb->sifs_info.sb_raw;

    // 校验魔数
    if (raw->magic != SIFS_FS_MAGIC_NUMBER) {
        return NULL;
    }

    // printk("Debug: root_ino = %d, magic = %x\n", raw->root_inode_no, raw->magic);

    // 同步 VFS 通用字段
    sb->s_block_size = SECTOR_SIZE;
    sb->s_root_ino = raw->root_inode_no;
    sb->s_magic = raw->magic;

    // 初始化位图
    uint32_t b_bm_sects = raw->block_bitmap_sects;
    sb->sifs_info.block_bitmap.btmp_bytes_len = b_bm_sects * SECTOR_SIZE;
    sb->sifs_info.block_bitmap.bits = kmalloc(sb->sifs_info.block_bitmap.btmp_bytes_len);
    partition_read(part, raw->block_bitmap_lba, sb->sifs_info.block_bitmap.bits, b_bm_sects);

    uint32_t i_bm_sects = raw->inode_bitmap_sects;
    sb->sifs_info.inode_bitmap.btmp_bytes_len = i_bm_sects * SECTOR_SIZE;
    sb->sifs_info.inode_bitmap.bits = kmalloc(sb->sifs_info.inode_bitmap.btmp_bytes_len);
    partition_read(part, raw->inode_bitmap_lba, sb->sifs_info.inode_bitmap.bits, i_bm_sects);

    // 建立根目录的 inode
    // 这一步会让 VFS 拥有入口点
    sb->s_root_inode = inode_open(part, sb->sifs_info.sb_raw.root_inode_no);
    
    if (sb->s_root_inode == NULL) {
        // 这里的异常处理需要释放位图内存
        // 但是目前我们先panic
        PANIC("sifs_read_super: fail to get inode");
        return NULL;
    }

    return sb;
}