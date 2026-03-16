#include <ext2_file.h>
#include <stdint.h>
#include <fs_types.h>
#include <errno.h>
#include <unistd.h>
#include <ide.h>
#include <stdio-kernel.h>

static int32_t ext2_readdir(struct inode* inode UNUSED, struct file* file, struct dirent* de, int count UNUSED) {
    struct inode* dir_inode = file->fd_inode;
    struct partition* part = get_part_by_rdev(dir_inode->i_dev);
    struct super_block* sb = dir_inode->i_sb;

    // 确定块大小
    uint32_t log_sz = sb->ext2_info.sb_raw.s_log_block_size;
    if (log_sz > 10) { // Ext2 规范通常 block 不超过 4KB (log=2)，给到 10 已经是极限了
        return -EIO; 
    }
    uint32_t block_size = EXT2_BLOCK_UNIT << log_sz;
    if (block_size == 0) return -EIO; // 拒绝除零

    uint8_t* block_buf = (uint8_t*)kmalloc(block_size);
    if (block_buf == NULL) return -ENOMEM;

    // 开始遍历，直到达到目录文件末尾
    while (file->fd_pos < dir_inode->i_size) {
        uint32_t cur_block_idx = file->fd_pos / block_size;
        // 获取当前逻辑块对应的物理块号
        uint32_t phys_block = dir_inode->i_op->bmap(dir_inode, cur_block_idx);
        if (phys_block == 0) {
            // 如果存在目录空洞，直接跳到下一个块
            file->fd_pos = (cur_block_idx + 1) * block_size;
            continue;
        }

        // 读取整个块
        partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), block_buf, block_size / SECTOR_SIZE);

        // 每一块的起始偏移取决于 fd_pos
        uint32_t offset_in_block = file->fd_pos % block_size;

        // 在块内遍历变长条目
        while (offset_in_block < block_size && file->fd_pos < dir_inode->i_size) {
            struct ext2_dir_entry* p_de = (struct ext2_dir_entry*)(block_buf + offset_in_block);

            // 如果磁盘损坏导致 rec_len 为 0，必须退出防止死循环
            if (p_de->rec_len == 0) {
                kfree(block_buf);
                return -EIO; 
            }

            // 暂存当前的偏移跳转量，因为不管是不是有效项，fd_pos 都要跳
            uint32_t current_rec_len = p_de->rec_len;
            
            if (p_de->i_no != 0) {
                // 找到有效条目，填充通用 dirent
                de->d_ino = p_de->i_no;
                de->d_type = p_de->file_type; // Ext2 的 file_type 与你的 enum 兼容
                de->d_off = file->fd_pos;
                de->d_reclen = sizeof(struct dirent);
                
                // 清空并拷贝文件名
                memset(de->d_name, 0, MAX_FILE_NAME_LEN);
                uint32_t name_copy_len = (p_de->name_len < MAX_FILE_NAME_LEN) ? p_de->name_len : (MAX_FILE_NAME_LEN - 1);
                memcpy(de->d_name, p_de->name, name_copy_len);
                
                // fd_pos 必须按磁盘上的 rec_len 对齐移动
                file->fd_pos += p_de->rec_len;
                
                kfree(block_buf);
                return 0; 
            }

            // 如果是已删除的空项，移动偏移量继续找
            file->fd_pos += current_rec_len;
            offset_in_block += current_rec_len;

            // 如果 fd_pos 因为加了 rec_len 跨块了，直接跳出内层去读新块
            if (file->fd_pos % block_size == 0) break;
        }
    }

    kfree(block_buf);
    return -1;
}

static int32_t ext2_file_read(struct inode* inode, struct file* file, char* buf, int32_t count) {
    struct partition* part = get_part_by_rdev(inode->i_dev);
    struct super_block* sb = inode->i_sb;
    
    // 确定块大小
    uint32_t block_size = EXT2_BLOCK_UNIT << sb->ext2_info.sb_raw.s_log_block_size;
    
    // 边界检查,不能读过文件末尾
    uint32_t size = count;
    if (file->fd_pos + count > inode->i_size) {
        size = inode->i_size - file->fd_pos;
    }
    
    if (size <= 0) {
        // 读完了或者参数不对
        return 0; 
    }

    // 申请一个块大小的缓冲区，用于处理非块对齐的读取
    uint8_t* io_buf = kmalloc(block_size);
    if (!io_buf) return -ENOMEM;

    uint32_t bytes_read = 0;
    uint32_t size_left = size;
    uint8_t* buf_dst = (uint8_t*)buf;

    while (bytes_read < size) {
        // 计算当前逻辑块号和块内偏移
        uint32_t block_idx = file->fd_pos / block_size;
        uint32_t offset_in_block = file->fd_pos % block_size;
        
        // 计算当前这轮循环能读多少字节
        uint32_t sec_left_in_block = block_size - offset_in_block;
        uint32_t chunk_size = (size_left < sec_left_in_block) ? size_left : sec_left_in_block;

        // 利用 bmap 找到物理块号
        uint32_t phys_block = inode->i_op->bmap(inode, block_idx);

        if (phys_block == 0) {
            // Ext2 支持空洞文件（Sparse File），如果块号为 0，填充 0
            memset(buf_dst, 0, chunk_size);
        } else {
            // 读取整个块到 io_buf
            partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, block_size / SECTOR_SIZE);
            memcpy(buf_dst, io_buf + offset_in_block, chunk_size);
        }

        // 更新状态
        buf_dst += chunk_size;
        file->fd_pos += chunk_size;
        bytes_read += chunk_size;
        size_left -= chunk_size;
    }

    kfree(io_buf);
    return bytes_read;
}

struct file_operations ext2_file_operations = {
	.lseek 		= NULL,
	.read 		= ext2_file_read,
	.write 		= NULL,
	.readdir 	= ext2_readdir,
	.ioctl 		= NULL,
	.open 		= NULL,
	.release 	= NULL
};