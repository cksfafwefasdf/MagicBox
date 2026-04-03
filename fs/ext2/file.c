#include <ext2_file.h>
#include <stdint.h>
#include <fs_types.h>
#include <errno.h>
#include <unitype.h>
#include <ide.h>
#include <stdio-kernel.h>
#include <debug.h>
#include <ext2_inode.h>
#include <thread.h>
#include <vma.h>

static uint32_t ext2_mmap_prot_to_vm_flags(uint32_t prot) {
    uint32_t vma_flags = 0;
    if (prot & PROT_READ) {
        vma_flags |= VM_READ;
    }
    if (prot & PROT_WRITE) {
        vma_flags |= VM_WRITE;
    }
    if (prot & PROT_EXEC) {
        vma_flags |= VM_EXEC;
    }
    return vma_flags;
}

// Linux 的 mmap 的主线也就只是挂载一个 VMA，然后等待惰性分配
// 除此之外，他还会挂载一个 VMA 的 VM 操作集，但是我们这没做 VM 操作集所以这一步就省略了
static int32_t ext2_file_mmap(struct inode* inode, struct file* file UNUSED,
                              uint32_t addr, uint32_t len, uint32_t prot,
                              uint32_t flags, uint32_t offset) {
    if (inode == NULL || inode->i_type != FT_REGULAR) {
        return -EINVAL;
    }
    if ((flags & MAP_PRIVATE) != MAP_PRIVATE || (flags & MAP_ANON) != 0) {
        return -EINVAL;
    }
    if ((offset & (PG_SIZE - 1)) != 0) {
        return -EINVAL;
    }

    // 建立映射视为一次访问
    // inode->i_atime = (uint32_t)sys_time();

    uint32_t file_visible_bytes = 0;
    if (offset < inode->i_size) {
        file_visible_bytes = inode->i_size - offset;
        if (file_visible_bytes > len) {
            file_visible_bytes = len;
        }
    }

    add_vma(get_running_task_struct(), addr, addr + len, offset, inode,
            ext2_mmap_prot_to_vm_flags(prot), file_visible_bytes);
    return 0;
}

static int32_t ext2_readdir(struct inode* inode UNUSED, struct file* file, struct dirent* de, int count UNUSED) {
    struct inode* dir_inode = file->fd_inode;
    struct partition* part = get_part_by_rdev(dir_inode->i_dev);
    struct super_block* sb = dir_inode->i_sb;

    // printk("DEBUG_READDIR: fd_pos=%d, i_size=%d\n", file->fd_pos, dir_inode->i_size);

    // 确定块大小
    uint32_t log_sz = sb->ext2_info.sb_raw.s_log_block_size;
    if (log_sz > 10) { // Ext2 规范通常 block 不超过 4KB (log=2)，给到 10 已经是极限了
        return -EIO; 
    }
    uint32_t block_size = EXT2_BLOCK_UNIT << log_sz;
    if (block_size == 0) return -EIO; // 拒绝除零

    uint8_t* block_buf = (uint8_t*)kmalloc(block_size);
    if (block_buf == NULL) return -ENOMEM;

    // 只有当从头开始读取目录时，更新一次 atime 即可
    // if (file->fd_pos == 0) {
    //     dir_inode->i_atime = (uint32_t)sys_time();
    //     // 同样，这里只更新内存，不强制刷盘
    // }

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
                de->d_type = p_de->file_type; // Ext2 的 file_type 与 enum 类型兼容
                de->d_off = file->fd_pos;
                de->d_reclen = sizeof(struct dirent);
                
                // 清空并拷贝文件名
                memset(de->d_name, 0, MAX_FILE_NAME_LEN);
                // 如果文件名超过vfs的支持，那么就截断
                // 目前有一个问题，ext2使用的是变长目录项，它文件名的最大长度是255，但是我们vfs最大只支持64
                // 因此此处可能会发生截断
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

// read 操作确实会改变atime，但是它不会里面同步回去，因此只有在进行write操作时才会写回去
// 例如我们先cat，后echo，后面那个echo会把前面那个cat改变的atime写回去，但是我们要是先echo，后cat，这个时间就不会写回去了
// 这点先需要注意一下，以后需要改进，目前就先这样吧
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

    // 只有当真正读到了数据（bytes_read > 0）时才更新
    // if (bytes_read > 0) {
    //     inode->i_atime = (uint32_t)sys_time();
        
    //     // 这里通常不需要立即调用 sb->s_op->write_inode(inode)。
    //     // atime 的修改通常只停留在内存中，
    //     // 等到 inode 周期性同步或者文件关闭时再写回磁盘。
    // }

    kfree(io_buf);
    return bytes_read;
}

static int32_t ext2_generic_lseek(struct inode* inode, struct file* pf, int32_t offset, int whence) {
    int32_t new_pos = 0;
    uint32_t file_size = inode->i_size;

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = (int32_t)pf->fd_pos + offset;
            break;
        case SEEK_END:
            new_pos = (int32_t)file_size + offset;
            break;
        default:
            return -EINVAL;
    }

    // 基础边界检查：不能偏移到文件开头之前
    if (new_pos < 0) {
        return -EINVAL;
    }

    // 针对目录的特殊限制
    // 在 Ext2 中，目录的 fd_pos 必须是 4 字节对齐的（或者必须指向有效的 dir_entry 开头）
    // 为了简单，我们要求目录的 lseek 不能超过 size
    if (inode->i_type == FT_DIRECTORY) {
        if ((uint32_t)new_pos > file_size) {
            return -EINVAL;
        }
    }

    // 针对普通文件的扩展 (Sparse File 支持)
    // 理论上普通文件可以 lseek 超过 i_size，但我们的系统中还没写好“空洞”自动填充逻辑
    // 因此可以先沿用SIFS里面的那种严格限制：
    if (inode->i_type == FT_REGULAR) {
        if ((uint32_t)new_pos > file_size) {
             // 若是支持稀疏文件，这里就不报错，直接让写函数去扩容
             return -EINVAL; 
        }
    }

    pf->fd_pos = new_pos;
    return pf->fd_pos;
}

static int32_t ext2_file_write(struct inode* inode, struct file* file,char* buf,int32_t count) {
    ASSERT(inode == file->fd_inode);
    struct super_block* sb = inode->i_sb;
    struct partition* part = get_part_by_rdev(inode->i_dev);
    uint32_t block_size = sb->s_block_size;

    // 准备缓冲区 (处理跨块的读-改-写)
    uint8_t* io_buf = kmalloc(block_size);
    ASSERT(io_buf!=NULL);
    if (!io_buf) return -ENOMEM;

    uint32_t bytes_written = 0;
    uint32_t size_left = count;
    const uint8_t* src = (const uint8_t*)buf;

    // 循环写入
    while (size_left > 0) {
        // 文件读写时统一用fd_pos作为参考系
        uint32_t logical_idx = file->fd_pos / block_size;
        uint32_t offset_in_block = file->fd_pos % block_size;
        uint32_t space_in_block = block_size - offset_in_block;
        uint32_t chunk_size = (size_left < space_in_block) ? size_left : space_in_block;

        // 获取该逻辑块对应的物理块
        uint32_t phys_block = inode->i_op->bmap(inode, logical_idx);

        // 如果物理块不存在，说明需要分配（扩容）
        if (phys_block == 0) {
            phys_block = ext2_resource_alloc(sb, 0, EXT2_BLOCK_BITMAP);
            ASSERT((int)phys_block!=-1);
            if ((int)phys_block == -1) {
                // 磁盘满了
                break; 
            }
            // 使用 append 函数将相应的块挂载到三级间址树上
            // append_block 内部会更新 i_size 和 i_blocks，
            if (ext2_append_block_to_inode(inode, phys_block) < 0) {
                // 回滚逻辑
                PANIC("fail to ext2_append_block_to_inode");
                break;
            }
            memset(io_buf, 0, block_size);
        } else {
            // 只有当不是完整覆盖这一整块时，才需要读-改-写
            // 完整覆盖的条件是：从 0 开始写，且写够 block_size
            if (!(offset_in_block == 0 && chunk_size == block_size)) {
                partition_read(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, block_size / SECTOR_SIZE);
            }
        }

        PUTS("write at: ",phys_block);
        // 读-改-写逻辑

        memcpy(io_buf + offset_in_block, src, chunk_size);
        partition_write(part, BLOCK_TO_SECTOR(sb, phys_block), io_buf, block_size / SECTOR_SIZE);

        // 更新位置和大小
        // 如果 fd_pos + chunk_size 超过了当前的 i_size，才更新i_size
        // 也就是说只有当“当前写入的位置”超过了“原有的文件大小”时，才推着 i_size 往前走
        file->fd_pos += chunk_size; // 先移动指针
        if (file->fd_pos > inode->i_size) {
            inode->i_size = file->fd_pos; // 只有写过了末尾才更新大小
        }

        // 更新计数
        src += chunk_size;
        bytes_written += chunk_size;
        size_left -= chunk_size;

    }

    // if (bytes_written > 0) {
    //     uint32_t now = (uint32_t)sys_time();
    //     inode->i_mtime = now;
    //     inode->i_ctime = now;
    // }

    // 持久化 Inode
    // 由于 sb 和 块组描述符里面都维护了很多实时信息，所以都要更新
    // 目前我们先这么写，按理说我们并不需要每一次写文件都要去动他们
    // 后期如果速度太慢了再来优化它
    sb->s_op->write_inode(inode);
    ext2_sync_gdt(sb);
    sb->s_op->write_super(sb);

    kfree(io_buf);
    return bytes_written;
}

struct file_operations ext2_file_file_operations = {
	.lseek 		= ext2_generic_lseek,
	.read 		= ext2_file_read,
	.write 		= ext2_file_write,
	.readdir 	= NULL,
	.ioctl 		= NULL,
	.open 		= NULL,
	.release 	= NULL,
    .mmap		= ext2_file_mmap
};

struct file_operations ext2_dir_file_operations = {
	.lseek 		= ext2_generic_lseek,
	.read 		= NULL,
	.write 		= NULL,
	.readdir 	= ext2_readdir,
	.ioctl 		= NULL,
	.open 		= NULL,
	.release 	= NULL,
    .mmap		= NULL
};
