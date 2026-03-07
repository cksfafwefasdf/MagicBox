#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <ioctl.h>
#include <sifs_sb.h>
#include <sifs_inode.h>
#include <sifs_fs.h>
#include <syscall.h>

// 根据内核定义确保这些常量一致
#define SECTOR_SIZE 512
#define BITS_PER_SECTOR (SECTOR_SIZE * 8)
#define DIV_ROUND_UP(X, STEP) (((X) + (STEP) - 1) / (STEP))
#define MAX_FILES_PER_PART 4096

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mkfs.sifs <device_path> (e.g. /dev/sdb1)\n");
        return -1;
    }

    // 打开设备
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) {
        printf("Error: Could not open device\n");
        return -1;
    }

    // 获取分区扇区数
    uint32_t sec_cnt = 0;
    if (ioctl(fd, BLKGETSIZE, (uint32_t)&sec_cnt) < 0) {
        printf("Error: ioctl BLKGETSIZE failed\n");
        close(fd);
        return -1;
    }

    // 计算布局 (与内核 sifs_format 逻辑严格对齐)
    uint32_t boot_sector_sects = 1;
    uint32_t super_block_sects = 1;
    uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);
    uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct sifs_inode) * MAX_FILES_PER_PART)), SECTOR_SIZE);
    
    uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
    uint32_t free_sects = sec_cnt - used_sects;

    // 计算 block bitmap 所需空间
    uint32_t block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
    uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects;
    block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR);

    // 构造超级块
    struct sifs_super_block sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = SIFS_FS_MAGIC_NUMBER;
    sb.sec_cnt = sec_cnt;
    sb.inode_cnt = MAX_FILES_PER_PART;
    
    // 相对偏移设置
    sb.block_bitmap_lba = 2; // OBR(0) + SB(1)
    sb.block_bitmap_sects = block_bitmap_sects;
    sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
    sb.inode_bitmap_sects = inode_bitmap_sects;
    sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
    sb.inode_table_sects = inode_table_sects;
    sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
    
    sb.root_inode_no = 0;
    sb.dir_entry_size = sizeof(struct sifs_dir_entry);

    // 准备内存缓冲区 (取最大元数据块的大小)
    uint32_t max_sects = sb.inode_table_sects > sb.block_bitmap_sects ? sb.inode_table_sects : sb.block_bitmap_sects;
    uint8_t* buf = (uint8_t*)malloc(max_sects * SECTOR_SIZE);
    if (!buf) { printf("Error: Out of memory\n"); return -1; }

    // 写入超级块
    lseek(fd, 1 * SECTOR_SIZE, SEEK_SET);
    write(fd, &sb, SECTOR_SIZE);

    // 初始化并写入 Block Bitmap
    memset(buf, 0, max_sects * SECTOR_SIZE);
    buf[0] |= 0x01; // 第0块预留给根目录数据
    // 处理末尾 padding (将超出的位设为1，表示不可用)
    uint32_t last_byte = block_bitmap_bit_len / 8;
    uint8_t last_bit = block_bitmap_bit_len % 8;
    for (uint32_t b = last_byte + 1; b < sb.block_bitmap_sects * SECTOR_SIZE; b++) buf[b] = 0xff;
    for (uint8_t bit = last_bit; bit < 8; bit++) buf[last_byte] |= (1 << bit);
    
    lseek(fd, sb.block_bitmap_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.block_bitmap_sects * SECTOR_SIZE);

    // 初始化并写入 Inode Bitmap
    memset(buf, 0, max_sects * SECTOR_SIZE);
    buf[0] |= 0x01; // 0号号Inode预留给根目录
    lseek(fd, sb.inode_bitmap_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.inode_bitmap_sects * SECTOR_SIZE);

    // 初始化并写入 Inode Table
    memset(buf, 0, max_sects * SECTOR_SIZE);
    struct sifs_inode* root_i = (struct sifs_inode*)buf;
    root_i->i_size = sb.dir_entry_size * 2; // "." 和 ".."
    root_i->i_type = FT_DIRECTORY;
    root_i->sii.i_sectors[0] = sb.data_start_lba;
    
    lseek(fd, sb.inode_table_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, sb.inode_table_sects * SECTOR_SIZE);

    // 写入根目录数据块
    memset(buf, 0, max_sects * SECTOR_SIZE);
    struct sifs_dir_entry* de = (struct sifs_dir_entry*)buf;
    // "." 目录
    memcpy(de[0].filename, ".", 1);
    de[0].i_no = 0;
    de[0].f_type = FT_DIRECTORY;
    // ".." 目录
    memcpy(de[1].filename, "..", 2);
    de[1].i_no = 0;
    de[1].f_type = FT_DIRECTORY;

    lseek(fd, sb.data_start_lba * SECTOR_SIZE, SEEK_SET);
    write(fd, buf, SECTOR_SIZE); // 根目录占1个扇区

    // 收尾
    free(buf);
    close(fd);
    printf("SIFS successfully formatted on %s\n", argv[1]);
    return 0;
}