#include <ext2_inode.h>
#include <ext2_fs.h>
#include <ext2_file.h>
#include <fs_types.h>
#include <errno.h>
#include <stdio-kernel.h>
#include <print.h>
#include <ide.h>
#include <inode.h>


static int32_t ext2_bmap(struct inode* inode, int32_t _index) {
    // 为了对齐接口，参数我们写成了int32_t类型，但是该函数全当无符号类型用 
    uint32_t index = (uint32_t) _index; 
    struct super_block* sb = inode->i_sb;
    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    uint32_t pnts_per_block = block_size / 4; // 每个块能容纳的指针数
    uint32_t* buf = kmalloc(block_size);
    uint32_t phys_block = 0;

    if (!buf) return 0;

    // 直接块 (0 - 11)
    if (index < 12) {
        phys_block = inode->ext2_i.i_block[index];
        goto out;
    }
    index -= 12;

    // 一级间接块 (12)
    if (index < pnts_per_block) {
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[12]), buf, block_size/SECTOR_SIZE);
        phys_block = buf[index];
        goto out;
    }
    index -= pnts_per_block;

    // 二级间接块 (13)
    if (index < pnts_per_block * pnts_per_block) {
        // 读取一级索引表（顶级）
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[13]), buf, block_size/SECTOR_SIZE);
        uint32_t first_idx = index / pnts_per_block;
        uint32_t second_idx = index % pnts_per_block;
        
        // 读取二级索引表
        partition_read(part, BLOCK_TO_SECTOR(sb, buf[first_idx]), buf, block_size/SECTOR_SIZE);
        phys_block = buf[second_idx];
        goto out;
    }
    index -= (pnts_per_block * pnts_per_block);

    // 三级间接块 (14) 
    // 最大范围可达 pnts_per_block^3，对于 1KB 块就是 1^3 = 64MB (太小)，但对于 4KB 块就是 1024^3 = 4TB
    uint32_t pnts_per_level2 = pnts_per_block * pnts_per_block;
    if (index < pnts_per_level2 * pnts_per_block) {
        // 读取一级索引表
        partition_read(part, BLOCK_TO_SECTOR(sb, inode->ext2_i.i_block[14]), buf, block_size/512);
        uint32_t first_idx = index / pnts_per_level2;
        uint32_t remain = index % pnts_per_level2;

        // 读取二级索引表
        uint32_t next_block = buf[first_idx];
        //  如果发现中途某个块地址为 0（即 next_block == 0），说明文件存在“空洞”（Sparse File），此时应该直接返回 0。
        if (next_block == 0) goto out; 
        partition_read(part, BLOCK_TO_SECTOR(sb, next_block), buf, block_size/512);
        uint32_t second_idx = remain / pnts_per_block;
        uint32_t third_idx = remain % pnts_per_block;

        // 读取三级索引表
        next_block = buf[second_idx];
        //  如果发现中途某个块地址为 0（即 next_block == 0），说明文件存在“空洞”（Sparse File），此时应该直接返回 0。
        if (next_block == 0) goto out;
        partition_read(part, BLOCK_TO_SECTOR(sb, next_block), buf, block_size/512);
        phys_block = buf[third_idx];
        goto out;
    }

out:
    kfree(buf);
    return phys_block;
}

static int ext2_lookup(struct inode* dir, char* name, int len, struct inode** res) {
    struct partition* part = get_part_by_rdev(dir->i_dev);
    struct super_block* sb = dir->i_sb;
    
    // 确定 Block Size
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    
    // 计算该目录文件一共有多少个逻辑块
    // 目录的大小由 i_size 决定，我们需要遍历所有逻辑块
    uint32_t total_blocks = (dir->i_size + block_size - 1) / block_size;

    char* buf = (char*)kmalloc(block_size);
    if (!buf) {
        put_str("ext2_lookup: fail to kmalloc for buf\n");
        return -ENOMEM;
    }

    // 使用bmap遍历所有逻辑块
    for (uint32_t i = 0; i < total_blocks; i++) {
        uint32_t phys_block = ext2_bmap(dir, i);
        
        // 如果返回 0，说明这个逻辑块是个空洞（在目录中很少见，但需处理）
        if (phys_block == 0) continue;

        // 读取物理块数据
        partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), buf, block_size / SECTOR_SIZE);

        struct ext2_dir_entry* de = (struct ext2_dir_entry*)buf;
        uint32_t offset = 0;

        // 在当前块内遍历所有目录项
        while (offset < block_size) {
            // 匹配条件是 Inode 不为0 且 长度匹配 且 内容匹配
            if (de->i_no != 0 && de->name_len == len && 
                memcmp(name, de->name, len) == 0) {
                
                // 找到编号，利用 VFS 的 inode_open 打开它
                struct inode* target_inode = inode_open(part, de->i_no);
                if (!target_inode) {
                    kfree(buf);
                    return -EIO;
                }

                *res = target_inode;
                kfree(buf);
                return 0; // 成功找到
            }

            // 异常保护：防止死循环
            if (de->rec_len == 0) {
                put_str("ext2_lookup: dir_entry rec_len is 0, disk corrupted!\n");
                break;
            }

            // 按 rec_len 跳转（Ext2 中的目录项是可以变长的）
            offset += de->rec_len;
            de = (struct ext2_dir_entry*)((uint8_t*)buf + offset);
        }
    }

    // 遍历完所有块都没找到
    kfree(buf);
    *res = NULL;
    return -ENOENT; 
}

void ext2_lookup_test(struct partition* part) {
    printk("\n--- [Ext2 Lookup Test: Searching for 'test.txt'] ---\n");

    // 获取根目录 Inode (Ext2 根目录固定为 2)
    struct inode* root_inode = inode_open(part, 2);
    if (!root_inode) {
        printk("Test Failed: Cannot open root inode!\n");
        return;
    }

    // 调用 ext2_lookup 查找目录项
    struct inode* res_inode = NULL;
    char* target_name = "test_dir";
    int ret = ext2_lookup(root_inode, target_name, strlen(target_name), &res_inode);

    // 结果验证
    if (ret == 0 && res_inode != NULL) {
        printk("Success! Found '%s' at Inode %d\n", target_name, res_inode->i_no);
        printk("File Size: %d bytes\n", res_inode->i_size);
        
        // 验证内容是否还是那句 "hello ext2 world"
        uint32_t first_block = ext2_bmap(res_inode, 0);
        if (first_block != 0) {
            char* buf = kmalloc(1024);
            partition_read(part, BLOCK_TO_SECTOR(res_inode->i_sb, first_block), buf, 2);
            printk("Verified Content: %s\n", buf);
            kfree(buf);
        }

        // 记得关闭查找到的 inode，否则会残留在缓存里
        inode_close(res_inode);
    } else {
        printk("Search Failed: '%s' not found. Error code: %d\n", target_name, ret);
    }

    inode_close(root_inode);
}

struct inode_operations ext2_inode_operations = {
    .default_file_ops = &ext2_file_operations,
    // .default_file_ops = NULL,
    .create     = NULL,
    .lookup     = ext2_lookup,
    .unlink     = NULL,
    .mkdir      = NULL,
    .rmdir      = NULL,
    .mknod      = NULL,
    .rename     = NULL,
    .bmap       = ext2_bmap, 
};