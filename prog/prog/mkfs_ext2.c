#include <stdio.h>
#include <string.h>
#include <unitype.h>
#include <stdint.h>
#include <ioctl.h>
#include <ext2_sb.h>
#include <ext2_inode.h>
#include <ext2_fs.h>
#include <syscall.h>

#define EXT2_INODES_PER_GROUP 1024
#define EXT2_BLOCKS_PER_GROUP 8192
#define EXT2_INODE_SIZE 128
#define EXT2_ROOT_INO 2
#define DEFAULT_EXT2_BLOCK_SIZE 1024

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mkfs.ext2 <device_path>\n");
        return -1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { printf("fail to open!\n"); return -1; }

    uint32_t sec_cnt = 0;
    // 获取块设备大小
    if (ioctl(fd, BLKGETSIZE, (uint32_t)&sec_cnt) < 0) {
        // 如果是普通文件而不是设备，用 lseek 算大小
        sec_cnt = lseek(fd, 0, SEEK_END) / 512;
    }

    uint32_t block_count = (sec_cnt * 512) / DEFAULT_EXT2_BLOCK_SIZE;

    // 计算动态组数
    uint32_t group_count = (block_count + EXT2_BLOCKS_PER_GROUP - 1) / EXT2_BLOCKS_PER_GROUP;
    uint32_t gdt_size_bytes = group_count * sizeof(struct ext2_group_desc);
    uint32_t gdt_blocks = (gdt_size_bytes + DEFAULT_EXT2_BLOCK_SIZE - 1) / DEFAULT_EXT2_BLOCK_SIZE;

    // 初始化超级块
    struct ext2_super_block sb = {0};
    sb.s_inodes_count = group_count * EXT2_INODES_PER_GROUP;
    sb.s_blocks_count = block_count;
    sb.s_r_blocks_count = block_count / 20;
    sb.s_free_inodes_count = sb.s_inodes_count - 11; 
    sb.s_free_blocks_count = block_count; 
    sb.s_first_data_block = 1; 
    sb.s_log_block_size = 0;   // 1KB
    sb.s_blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    sb.s_inodes_per_group = EXT2_INODES_PER_GROUP;
    sb.s_magic = EXT2_MAGIC_NUMBER;
    sb.s_state = 1;
    sb.s_rev_level = 1;
    sb.s_inode_size = EXT2_INODE_SIZE;
    sb.s_first_ino = 11;
    sb.s_feature_incompat = 0x02;
    sb.s_log_frag_size = 0;
    sb.s_errors = 1; // EXT2_ERRORS_CONTINUE (发现错误继续运行，这是标准值)
    sb.s_creator_os = 0; // Linux
    sb.s_max_mnt_count = 20; // 最大挂载次数
    sb.s_checkinterval = 0; // 不强制定期检查
    sb.s_wtime = 1710900000; // 随便写一个最近的 Unix 时间戳,防止报错
    sb.s_frags_per_group = sb.s_blocks_per_group; // 这一句一定要写，不然 e2fsck 会报错！

    // 构造组描述符表 (GDT)
    struct ext2_group_desc* gdt = calloc(group_count, sizeof(struct ext2_group_desc));
    uint32_t itable_blocks = (EXT2_INODES_PER_GROUP * EXT2_INODE_SIZE) / DEFAULT_EXT2_BLOCK_SIZE;
    uint32_t total_used_blocks = 0;
    for (uint32_t i = 0; i < group_count; i++) {
        uint32_t group_base = i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block;
        uint32_t offset = 1 + gdt_blocks;
        gdt[i].bg_block_bitmap = group_base + offset;
        gdt[i].bg_inode_bitmap = group_base + offset + 1;
        gdt[i].bg_inode_table  = group_base + offset + 2;
        
        uint32_t used_meta = offset + 2 + itable_blocks;
        if (i == 0) used_meta++; // 根目录数据块

        gdt[i].bg_free_blocks_count = (i == group_count - 1) ? 
            (block_count - (i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block) - used_meta) : 
            (EXT2_BLOCKS_PER_GROUP - used_meta);
        gdt[i].bg_free_inodes_count = (i == 0) ? (EXT2_INODES_PER_GROUP - 11) : EXT2_INODES_PER_GROUP;
        gdt[i].bg_used_dirs_count = (i == 0) ? 1 : 0;

        total_used_blocks += used_meta;
    }

    sb.s_free_blocks_count = block_count - total_used_blocks;

    // 写入元数据
    lseek(fd, DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, &sb, sizeof(sb));
    lseek(fd, 2 * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, gdt, gdt_size_bytes);

    // 循环初始化块组
    uint8_t* buf = calloc(1, DEFAULT_EXT2_BLOCK_SIZE);
    for (uint32_t i = 0; i < group_count; i++) {
        // 块位图
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        uint32_t meta_in_group = (1 + gdt_blocks + 2 + itable_blocks);
        for (uint32_t b = 0; b < meta_in_group; b++) buf[b/8] |= (1 << (b%8));
        if (i == 0) buf[meta_in_group/8] |= (1 << (meta_in_group%8));
        
        if (i == group_count - 1) {
            uint32_t last_group_blocks = block_count - (i * EXT2_BLOCKS_PER_GROUP + sb.s_first_data_block);
            for (uint32_t b = last_group_blocks; b < 8192; b++) buf[b/8] |= (1 << (b%8));
        }
        lseek(fd, (int32_t)gdt[i].bg_block_bitmap * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

        // Inode 位图
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        if (i == 0) {
            for (int n = 0; n < 11; n++) buf[n/8] |= (1 << (n%8));
        }
        lseek(fd, (int32_t)gdt[i].bg_inode_bitmap * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

        // 清空 Inode Table
        memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
        lseek(fd, (int32_t)gdt[i].bg_inode_table * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
        for (uint32_t t = 0; t < itable_blocks; t++) write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);
    }

    // 根目录初始化
    uint32_t root_data_block = gdt[0].bg_inode_table + itable_blocks; 
    struct ext2_inode root_inode = {0};
    root_inode.i_mode = 0x41ED; // drwxr-xr-x
    root_inode.i_size = DEFAULT_EXT2_BLOCK_SIZE;
    root_inode.i_links_count = 2;
    root_inode.i_blocks = 2; // 1KB block = 2 sectors
    root_inode.i_block[0] = root_data_block;
    
    lseek(fd, (int32_t)gdt[0].bg_inode_table * DEFAULT_EXT2_BLOCK_SIZE + (EXT2_ROOT_INO - 1) * EXT2_INODE_SIZE, SEEK_SET);
    write(fd, &root_inode, EXT2_INODE_SIZE);

    memset(buf, 0, DEFAULT_EXT2_BLOCK_SIZE);
    struct ext2_dir_entry* de = (struct ext2_dir_entry*)buf;
    // "." 条目
    de->i_no = EXT2_ROOT_INO;
    de->rec_len = 12;
    de->name_len = 1;
    de->file_type = 2;
    strcpy(de->name, ".");
    // ".." 条目
    de = (void*)de + de->rec_len;
    de->i_no = EXT2_ROOT_INO;
    de->rec_len = DEFAULT_EXT2_BLOCK_SIZE - 12; // 覆盖剩余空间
    de->name_len = 2;
    de->file_type = 2;
    strcpy(de->name, "..");

    lseek(fd, (int32_t)root_data_block * DEFAULT_EXT2_BLOCK_SIZE, SEEK_SET);
    write(fd, buf, DEFAULT_EXT2_BLOCK_SIZE);

    
    free(gdt); free(buf); close(fd);
    printf("mkfs.ext2 done!\n");
    return 0;
}