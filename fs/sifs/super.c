#include <fs_types.h>
#include <ide_buffer.h>
#include <fs.h>
#include <ide.h>
#include <debug.h>
#include <global.h>
#include <stdio-kernel.h>
#include <inode.h>
#include <memory.h>
#include <unitype.h>

int32_t inode_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->sb->sifs_info.inode_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->sb->sifs_info.inode_bitmap,bit_idx,1);
	return bit_idx;
}

int32_t block_bitmap_alloc(struct partition* part){
	int32_t bit_idx = bitmap_scan(&part->sb->sifs_info.block_bitmap,1);
	if(bit_idx==-1){
		return -1;
	}
	bitmap_set(&part->sb->sifs_info.block_bitmap,bit_idx,1);
	return (part->sb->sifs_info.sb_raw.data_start_lba+bit_idx);
}

// write the sector containing the bit_idx of the in-memory bitmap back to disk
void bitmap_sync(struct partition* part,uint32_t bit_idx,enum bitmap_type btmp_type){
	// Since bit_idx is in bits, we first convert it to bytes by dividing by 8
	uint32_t off_sec = bit_idx/(8*SECTOR_SIZE); 

	uint32_t off_size = off_sec*SECTOR_SIZE;

	uint32_t sec_lba;
	uint8_t* bitmap_off;

	switch (btmp_type){
		case INODE_BITMAP:
			sec_lba = part->sb->sifs_info.sb_raw.inode_bitmap_lba+off_sec;
			bitmap_off = part->sb->sifs_info.inode_bitmap.bits+off_size;
			break;
		case BLOCK_BITMAP:
			sec_lba = part->sb->sifs_info.sb_raw.block_bitmap_lba+off_sec;
			bitmap_off = part->sb->sifs_info.block_bitmap.bits+off_size;
			break;
		default:
			PANIC("unknown bitmap type!!!\n");
			return;
	}
	partition_write(part,sec_lba,bitmap_off,1);
}


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

static struct super_block * sifs_read_super(struct super_block *sb, void *data UNUSED, int silent) {
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

	// 挂载操作集
	sb->s_op = &sifs_super_ops;

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

// Calculate the logical sector offset and byte offset within the sector by inode number
static void inode_locate(struct partition* part,uint32_t inode_no,struct inode_position* inode_pos){

	ASSERT(inode_no<MAX_FILES_PER_PART);
	uint32_t inode_table_lba = part->sb->sifs_info.sb_raw.inode_table_lba;

	uint32_t inode_size = sizeof(struct sifs_inode);
	// total bytes offset
	uint32_t off_size = inode_no*inode_size;

	uint32_t off_sec = off_size/SECTOR_SIZE;
	// offset in sectors
	uint32_t off_size_in_sec = off_size%SECTOR_SIZE;
	// remaining sector space from off_size_in_sec
	uint32_t left_in_sec = SECTOR_SIZE-off_size_in_sec;
	// check if the inode spans two sectors
	if(left_in_sec<inode_size){
		inode_pos->two_sec = true;
	}else{
		inode_pos->two_sec = false;
	}
	inode_pos->sec_lba = inode_table_lba+off_sec;
	inode_pos->off_size = off_size_in_sec;
}

// Write the inode back to disk
// Sync the inode to disk
// 这个函数是sifs写回inode的唯一入口
// 在此之前，我们都只操作 inode 结构体上的 i_rdev, i_size, i_type
// 而不是操作 sifs_inode 上的
static void sifs_write_inode(struct inode *inode){
	// printk("inode->i_dev:%x  part->i_rdev:%x \n",inode->i_dev,part->i_rdev);
	struct partition* part = get_part_by_rdev(inode->i_dev);
	ASSERT(inode->i_dev == part->i_rdev);
	uint8_t inode_no = inode->i_no;
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);

	ASSERT(inode_pos.sec_lba<=(part->sec_cnt));

	char* inode_buf = (char*)kmalloc(SECTOR_SIZE*2);
	memset(inode_buf, 0, SECTOR_SIZE * 2);

	if(inode_buf==NULL){
		PANIC("sifs_write_inode: fail to kmalloc for inode_buf!");
	}

	uint8_t sec_to_write = inode_pos.two_sec?2:1;

	partition_read(part,inode_pos.sec_lba,inode_buf,sec_to_write);
	struct sifs_inode* si = (struct sifs_inode*)(inode_buf + inode_pos.off_size);
	si->i_type = inode->i_type;
	si->i_size = inode->i_size;
	si->sii = inode->sifs_i;
	// memcpy((inode_buf+inode_pos.off_size),&inode->sifs_i,sizeof(struct sifs_inode));
	partition_write(part,inode_pos.sec_lba,inode_buf,sec_to_write);
	// PUTS("write type: ",si->i_type);
	kfree(inode_buf);
}

static void sifs_read_inode(struct inode* inode) {
    struct inode_position inode_pos;
	struct partition* part = get_part_by_rdev(inode->i_dev);
    inode_locate(part, inode->i_no, &inode_pos);

    uint8_t sec_to_read = inode_pos.two_sec ? 2 : 1;
    char* inode_buf = (char*)kmalloc(SECTOR_SIZE * sec_to_read);
    
    partition_read(part, inode_pos.sec_lba, inode_buf, sec_to_read);
    
    struct sifs_inode* si = (struct sifs_inode*)(inode_buf + inode_pos.off_size);

    // 填充通用字段
    inode->i_size = si->i_size;
    inode->i_type = si->i_type;
    
    // 填充 SIFS 特有字段
    inode->sifs_i = si->sii;

    if (inode->i_type == FT_CHAR_SPECIAL || inode->i_type == FT_BLOCK_SPECIAL) {
        inode->i_rdev = si->sii.i_rdev; 
    }

	// 根据文件类型赋予操作集
    switch (inode->i_type) {
        case FT_REGULAR:
            inode->i_op = &sifs_file_inode_operations;
            break;
        case FT_DIRECTORY:
            inode->i_op = &sifs_dir_inode_operations;
            break;
        case FT_CHAR_SPECIAL:
            inode->i_op = &sifs_char_inode_operations;
            break;
        case FT_BLOCK_SPECIAL:
            inode->i_op = &sifs_block_inode_operations;
            break;
        default:
            inode->i_op = NULL;
            break;
    }

    kfree(inode_buf);
}

// 用于在umount时，释放和文件系统相关的内存资源
// 例如FAT中是内存表，sifs中就是位图
static void sifs_put_super(struct super_block *sb){
    if (sb->sifs_info.block_bitmap.bits) {
        kfree(sb->sifs_info.block_bitmap.bits);
        sb->sifs_info.block_bitmap.bits = NULL;
    }
    if (sb->sifs_info.inode_bitmap.bits) {
        kfree(sb->sifs_info.inode_bitmap.bits);
        sb->sifs_info.inode_bitmap.bits = NULL;
    }
}

static void sifs_statfs(struct super_block *sb, struct statfs *buf) {
    struct sifs_super_block* sifs_sb = &sb->sifs_info.sb_raw;
	struct partition* part = get_part_by_rdev(sb->s_dev);

    // 填充静态信息
    buf->f_type    = sifs_sb->magic;
    buf->f_bsize   = SECTOR_SIZE;
    buf->f_blocks  = sifs_sb->sec_cnt;
    buf->f_namelen = MAX_PATH_LEN;

    // 现场计算动态信息
    uint32_t btmp_sects = sifs_sb->block_bitmap_sects;
    uint8_t* btmp_buf = kmalloc(btmp_sects * SECTOR_SIZE);

	if (btmp_buf == NULL) {
        PANIC("sifs_statfs: fail to kmalloc for btmp_buf");
    }
	
	partition_read(part, sifs_sb->block_bitmap_lba, btmp_buf, btmp_sects);

	struct bitmap btmp;
	btmp.bits = btmp_buf;
	btmp.btmp_bytes_len = sifs_sb->sec_cnt / 8;

	uint32_t free_sects = bitmap_count(&btmp);
	buf->f_bfree  = free_sects;
	buf->f_bavail = free_sects; // 单用户，全可用

	kfree(btmp_buf);
    
}

void sifs_inode_delete(struct partition* part,uint32_t inode_no,void* io_buf){
	ASSERT(inode_no<MAX_FILES_PER_PART);
	struct inode_position inode_pos;
	inode_locate(part,inode_no,&inode_pos);
	ASSERT(inode_pos.sec_lba<=(part->sec_cnt));

	char* inode_buf = (char*)io_buf;
	uint8_t sec_to_op = inode_pos.two_sec?2:1;

	partition_read(part,inode_pos.sec_lba,inode_buf,sec_to_op);
	memset((inode_buf+inode_pos.off_size),0,sizeof(struct sifs_inode));
	partition_write(part,inode_pos.sec_lba,inode_buf,sec_to_op);
	
}

// 这些函数全都是通过sifs_super_ops导出的，因此全部声明成static
struct super_operations sifs_super_ops = {
    .read_inode  = sifs_read_inode,
    .write_inode = sifs_write_inode, 
    .put_inode   = NULL, // 由于我们采用强制同步的方式，没有延迟写，因此直接在write_inode中就做完所有操作了，暂时不需要put_inode
    .put_super   = sifs_put_super, // 对应释放位图内存的逻辑
    .write_super = NULL, // 我们sifs的super_block里面的字段都是初始化时直接填好的，之后不会再变，因此我们不需要同步内存超级块的操作
    .statfs      = sifs_statfs        // 对应 df 命令用的统计逻辑
};

struct file_system_type sifs_fs_type = {
    .name = "sifs",
    .flags = FS_REQUIRES_DEV, // sifs是有设备对应的，不是内存文件系统
    .read_super = sifs_read_super 
};