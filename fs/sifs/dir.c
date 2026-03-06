#include <sifs_dir.h>
#include <sifs_inode.h>
#include <ide.h>
#include <stdio-kernel.h>
#include <string.h>
#include <debug.h>
#include <sifs_file.h>
#include <fs.h>
#include <ide_buffer.h>
#include <sifs_sb.h>
#include <inode.h>

// 由于我们去除了 dir 结构，因此现在改用 file 来标记根目录
struct inode* root_dir_inode; 

void open_root_dir(struct partition* part) {
    root_dir_inode = inode_open(part, part->sb->s_root_ino);
}

void close_root_dir(struct partition* part) {
    // 确保根目录确实存在且被打开了
    if (root_dir_inode != NULL) {
        // 调用 inode_close，它会处理引用计数和磁盘同步
        inode_close(root_dir_inode);
    }
}

bool sifs_search_dir_entry(struct partition* part, struct inode* dir_inode, const char* name, struct sifs_dir_entry* de) {
    // 计算所有可能的块地址（包括一级间接块）
    uint32_t block_cnt = DIRECT_INDEX_BLOCK + (SECTOR_SIZE / ADDR_BYTES_32BIT);
    uint32_t* all_blocks_addr = (uint32_t*)kmalloc(block_cnt * ADDR_BYTES_32BIT);
    if (all_blocks_addr == NULL) {
        printk("in search_dir_entry: kmalloc failed!\n");
        return false;
    }
    memset(all_blocks_addr, 0, block_cnt * ADDR_BYTES_32BIT);

    // 填充直接块地址
    uint32_t block_idx = 0;
    while (block_idx < DIRECT_INDEX_BLOCK) {
        all_blocks_addr[block_idx] = dir_inode->sifs_i.i_sectors[block_idx];
        block_idx++;
    }

    // 处理一级间接块
    uint32_t tfflib = DIRECT_INDEX_BLOCK;
    if (dir_inode->sifs_i.i_sectors[tfflib] != 0) {
        partition_read(part, dir_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    // 准备缓冲区开始遍历
    uint8_t* buf = (uint8_t*)kmalloc(SECTOR_SIZE);

    if (buf == NULL) {
        kfree(all_blocks_addr);
        return false;
    }

    uint32_t dir_entry_size = part->sb->sifs_info.sb_raw.dir_entry_size;
    uint32_t dir_entry_cnt = SECTOR_SIZE / dir_entry_size;

    block_idx = 0;
    while (block_idx < block_cnt) {
        if (all_blocks_addr[block_idx] == 0) {
            block_idx++;
            continue;
        }

        partition_read(part, all_blocks_addr[block_idx], buf, 1);
        struct sifs_dir_entry* p_de = (struct sifs_dir_entry*)buf;

        uint32_t dir_entry_idx = 0;
        while (dir_entry_idx < dir_entry_cnt) {
            // printk("p_de name:%s\n",p_de->filename);
            // 这里的 f_type != FT_UNKNOWN 是为了跳过已删除的条目
            if (name[0] != '\0' && p_de->f_type != FT_UNKNOWN && !strcmp(p_de->filename, name)) {

                memcpy(de, p_de, dir_entry_size);

                kfree(buf);
                kfree(all_blocks_addr);
                return true;
            }
            dir_entry_idx++;
            p_de++;
        }
        block_idx++;
    }

    kfree(buf);
    kfree(all_blocks_addr);
    return false;
}

// 这里的 p_de 必须指向从磁盘读出来的 sector 缓冲区，所以使用针对文件系统特化的 sifs_dir_entry
void sifs_create_dir_entry(char* filename, uint32_t inode_no, enum file_types file_type, struct sifs_dir_entry* p_de) {
    ASSERT(strlen(filename) <= MAX_FILE_NAME_LEN);

    // 清除旧数据，确保 filename 后面到结构体结束的部分全是 0
    memset(p_de, 0, sizeof(struct sifs_dir_entry));

    memcpy(p_de->filename, filename, strlen(filename));
    p_de->i_no = inode_no;
    p_de->f_type = file_type;
}

bool sifs_sync_dir_entry(struct inode* parent_inode, struct sifs_dir_entry* p_de, void* io_buf) {


    struct partition* part = get_part_by_rdev(parent_inode->i_dev);

#ifdef DEBUG_DIR
    PUTS("i_size before: ",parent_inode->i_size);
    PUTS("parent before: ",parent_inode);
    for(struct dlist_elem* pinode = part->open_inodes.head.next;pinode!=NULL;pinode=pinode->next){
        PUTS("list: ",pinode);
    }
#endif
    

    uint32_t dir_entry_size = part->sb->sifs_info.sb_raw.dir_entry_size;
    uint32_t dir_entrys_per_sec = (SECTOR_SIZE / dir_entry_size);
    uint32_t tfflib = DIRECT_INDEX_BLOCK; // 第一级间接块索引

    ASSERT(parent_inode->i_size % dir_entry_size == 0);

    // 搜集所有数据块地址 (包括处理间接块)
    uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};
    for (uint8_t i = 0; i < DIRECT_INDEX_BLOCK; i++) {
        all_blocks_addr[i] = parent_inode->sifs_i.i_sectors[i];
    }
    if (parent_inode->sifs_i.i_sectors[tfflib] != 0) {
        partition_read(part, parent_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    struct sifs_dir_entry* dir_e = (struct sifs_dir_entry*)io_buf;
    int32_t block_lba = -1;

    // 遍历父目录的所有块，寻找空位 (FT_UNKNOWN) 或分配新块
    for (uint32_t block_idx = 0; block_idx < TOTAL_BLOCK_COUNT; block_idx++) {
        // 若当前块未分配，需要申请新块
        if (all_blocks_addr[block_idx] == 0) {
            block_lba = block_bitmap_alloc(part);
            if (block_lba == -1) return false;

            uint32_t bitmap_idx = block_lba - part->sb->sifs_info.sb_raw.data_start_lba;
            bitmap_sync(part, bitmap_idx, BLOCK_BITMAP);

            // 更新索引逻辑
            if (block_idx < tfflib) {
                // 直接块
                parent_inode->sifs_i.i_sectors[block_idx] = all_blocks_addr[block_idx] = block_lba;
            } else if (block_idx == tfflib) {
                // 正好是间接块指针本身
                parent_inode->sifs_i.i_sectors[tfflib] = block_lba;
                block_lba = block_bitmap_alloc(part); // 为间接块指向的第一个数据块再分配一次
                if (block_lba == -1){
					// 此处逻辑应回滚，此处简化，直接panic占位
					PANIC("sifs_sync_dir_entry: fail to block_bitmap_alloc");
				}
                
                bitmap_idx = block_lba - part->sb->sifs_info.sb_raw.data_start_lba;
                bitmap_sync(part, bitmap_idx, BLOCK_BITMAP);
                all_blocks_addr[tfflib] = block_lba;
                // 同步间接块内容到磁盘
                partition_write(part, parent_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
            } else {
                // 间接块内的数据块
                all_blocks_addr[block_idx] = block_lba;
                partition_write(part, parent_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
            }

            // 在新块的开头写入条目
            memset(io_buf, 0, SECTOR_SIZE);
            memcpy(io_buf, p_de, dir_entry_size);
            partition_write(part, all_blocks_addr[block_idx], io_buf, 1);
            
            parent_inode->i_size += dir_entry_size;

#ifdef DEBUG_DIR
            PUTS("i_size after: ",parent_inode->i_size);
            PUTS("parent after: ",parent_inode);
#endif
            return true;
        }

        // 若当前块已存在，读取它寻找 FT_UNKNOWN 的空隙
        partition_read(part, all_blocks_addr[block_idx], io_buf, 1);
        for (uint8_t entry_idx = 0; entry_idx < dir_entrys_per_sec; entry_idx++) {
            if ((dir_e + entry_idx)->f_type == FT_UNKNOWN) {
                memcpy(dir_e + entry_idx, p_de, dir_entry_size);
                partition_write(part, all_blocks_addr[block_idx], io_buf, 1);
                
                parent_inode->i_size += dir_entry_size;

#ifdef DEBUG_DIR
                PUTS("i_size after: ",parent_inode->i_size);
                PUTS("parent after: ",parent_inode);
#endif
                return true;
            }
        }
    }

#ifdef DEBUG_DIR
    PUTS("i_size after: ",parent_inode->i_size);
#endif
    printk("sifs_sync_dir_entry: directory is full!\n");
    return false;
}

bool sifs_delete_dir_entry(struct partition* part, struct inode* parent_inode, uint32_t inode_no, void* io_buf) {
    uint32_t block_idx = 0;
    uint32_t all_blocks_addr[TOTAL_BLOCK_COUNT] = {0};

    // 获取所有块地址
    while (block_idx < DIRECT_INDEX_BLOCK) {
        all_blocks_addr[block_idx] = parent_inode->sifs_i.i_sectors[block_idx];
        block_idx++;
    }
    uint32_t tfflib = DIRECT_INDEX_BLOCK;
    if (parent_inode->sifs_i.i_sectors[tfflib]) {
        partition_read(part, parent_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    uint32_t dir_entry_size = part->sb->sifs_info.sb_raw.dir_entry_size;
    uint32_t dir_entrys_per_sec = (SECTOR_SIZE / dir_entry_size);
    struct sifs_dir_entry* dir_e = (struct sifs_dir_entry*)io_buf;
    struct sifs_dir_entry* dir_entry_found = NULL;
    
    uint8_t dir_entry_idx, dir_entry_cnt;
    bool is_dir_first_block = false;

    // 遍历查找
    block_idx = 0;
    while (block_idx < TOTAL_BLOCK_COUNT) {
        is_dir_first_block = false;
        if (all_blocks_addr[block_idx] == 0) {
            block_idx++;
            continue;
        }

        dir_entry_idx = dir_entry_cnt = 0;
        memset(io_buf, 0, SECTOR_SIZE);
        partition_read(part, all_blocks_addr[block_idx], io_buf, 1);

        // 检查当前块内的每一个条目
        while (dir_entry_idx < dir_entrys_per_sec) {
            struct sifs_dir_entry* cur_e = dir_e + dir_entry_idx;
            if (cur_e->f_type != FT_UNKNOWN) {
                // 判断是否是目录的起始块（含有 . 条目）
                if (!strcmp(cur_e->filename, ".")) {
                    is_dir_first_block = true;
                } else if (strcmp(cur_e->filename, ".") && strcmp(cur_e->filename, "..")) {
                    // 记录非特殊的有效条目数量
                    dir_entry_cnt++;
                    if (cur_e->i_no == inode_no) {
                        ASSERT(dir_entry_found == NULL);
                        dir_entry_found = cur_e;
                    }
                }
            }
            dir_entry_idx++;
        }

        if (dir_entry_found == NULL) {
            block_idx++;
            continue;
        }

        // 找到目标，执行删除逻辑
        
        // 如果该块除了被删条目外没有其他条目，且不是首块，则回收整个块
        if (dir_entry_cnt == 1 && !is_dir_first_block) {
            uint32_t block_bitmap_idx = all_blocks_addr[block_idx] - part->sb->sifs_info.sb_raw.data_start_lba;
            bitmap_set(&part->sb->sifs_info.block_bitmap, block_bitmap_idx, 0);
            bitmap_sync(part, block_bitmap_idx, BLOCK_BITMAP);

            if (block_idx < DIRECT_INDEX_BLOCK) {
                parent_inode->sifs_i.i_sectors[block_idx] = 0;
            } else {
                // 处理间接块逻辑（统计剩余间接数据块，若为0则回收间接块本身）
                uint32_t indirect_blocks = 0;
                for (uint32_t i = tfflib; i < TOTAL_BLOCK_COUNT; i++) {
                    if (all_blocks_addr[i] != 0) indirect_blocks++;
                }

                if (indirect_blocks > 1) {
                    all_blocks_addr[block_idx] = 0;
                    partition_write(part, parent_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
                } else {
                    // 回收一级间接块表所在的块
                    uint32_t idx = parent_inode->sifs_i.i_sectors[tfflib] - part->sb->sifs_info.sb_raw.data_start_lba;
                    bitmap_set(&part->sb->sifs_info.block_bitmap, idx, 0);
                    bitmap_sync(part, idx, BLOCK_BITMAP);
                    parent_inode->sifs_i.i_sectors[tfflib] = 0;
                }
            }
        } else {
            // 只是普通的条目擦除
            memset(dir_entry_found, 0, dir_entry_size);
            partition_write(part, all_blocks_addr[block_idx], io_buf, 1);
        }

        // 更新父目录 inode 信息并同步
        ASSERT(parent_inode->i_size >= dir_entry_size);
        // i_size 标记的是该目录文件目前所达到的最大逻辑偏移量。
        // 他只增不减，这是为了便于处理空洞，具体原因可以阅读i_size字段处的解释
        // parent_inode->i_size -= dir_entry_size;
        memset(io_buf, 0, SECTOR_SIZE * 2);
        inode_sync(part, parent_inode, io_buf);

        return true;
    }
    return false;
}
	
int sifs_dir_read(struct file* file, struct dirent* de) {
    struct inode* dir_inode = file->fd_inode;
    struct partition* part = get_part_by_rdev(dir_inode->i_dev);
    uint32_t dir_entry_size = part->sb->sifs_info.sb_raw.dir_entry_size;

    // 获取该目录 inode 占据的所有数据块地址
    uint32_t* all_blocks_addr = (uint32_t*)kmalloc(sizeof(uint32_t)*TOTAL_BLOCK_COUNT);
    uint32_t block_idx = 0;
    while (block_idx < DIRECT_INDEX_BLOCK) {
        all_blocks_addr[block_idx] = dir_inode->sifs_i.i_sectors[block_idx];
        block_idx++;
    }

    uint32_t tfflib = DIRECT_INDEX_BLOCK;
    if (dir_inode->sifs_i.i_sectors[tfflib] != 0) {
        partition_read(part, dir_inode->sifs_i.i_sectors[tfflib], all_blocks_addr + tfflib, 1);
    }

    // 分配临时扇区缓冲区 
	// 代替原来的 dir->dir_buf，原来我们的每一个dir结构体都自带缓冲区
	// 现在我们统一使用 file 了，不再使用 dir 了，因此得手动申请缓冲区了
    uint8_t* sector_buf = (uint8_t*)kmalloc(SECTOR_SIZE);
    if (sector_buf == NULL) {
        printk("sifs_dir_read: kmalloc failed\n");
        return -1;
    }

    // 开始遍历目录文件中的各个项
    // 我们需要跳过那些 f_type == FT_UNKNOWN 的“空洞”条目，直到找到一个有效的
    while (file->fd_pos < dir_inode->i_size) {
        
        // 计算当前 fd_pos 对应的块索引和块内偏移
        uint32_t cur_block_idx = file->fd_pos / SECTOR_SIZE;
        // uint32_t entry_offset_in_sec = (file->fd_pos % SECTOR_SIZE) / dir_entry_size;

        if (all_blocks_addr[cur_block_idx] == 0) {
            // 如果该块不存在，直接跳过这一整个块的范围
            // 这样做可以加速遍历，不需要一个字节一个字节挪
            file->fd_pos = (cur_block_idx + 1) * SECTOR_SIZE;
            continue;
        }

        // 读取当前扇区
        memset(sector_buf, 0, SECTOR_SIZE);
        partition_read(part, all_blocks_addr[cur_block_idx], sector_buf, 1);

        struct sifs_dir_entry* p_de = (struct sifs_dir_entry*)sector_buf;

        // 在当前扇区内继续寻找有效条目
        // 每次进入新扇区，内层循环的起始偏移基于 fd_pos 动态计算
        while (file->fd_pos < (cur_block_idx + 1) * SECTOR_SIZE && file->fd_pos < dir_inode->i_size) {
            uint32_t offset = (file->fd_pos % SECTOR_SIZE) / dir_entry_size;
            struct sifs_dir_entry* cur_sifs_de = p_de + offset;
            
            if (cur_sifs_de->f_type != FT_UNKNOWN) {
                // 将 SIFS 磁盘镜像转为通用 dirent
                de->d_ino = cur_sifs_de->i_no;
                de->d_type = cur_sifs_de->f_type;
                de->d_off = file->fd_pos;
                de->d_reclen = sizeof(struct dirent);
                memset(de->d_name, 0, MAX_FILE_NAME_LEN);
                strcpy(de->d_name, cur_sifs_de->filename);

                // 找到有效条目，fd_pos 前移，准备下一次读取
                file->fd_pos += dir_entry_size;
                kfree(sector_buf);
                kfree(all_blocks_addr);
                return 0; 
            }

            // 如果是无效条目，仅仅增加 fd_pos 并继续在扇区内查找
            file->fd_pos += dir_entry_size;
            
            // 安全检查：如果偏移超出了 inode 大小，直接结束
            if (file->fd_pos >= dir_inode->i_size) {
                kfree(sector_buf);
                kfree(all_blocks_addr);
                return -1;
            }
        }
        // 如果这个扇区找完了都没找到有效条目，while 循环会进入下一个 block_idx
    }

    kfree(sector_buf);
    kfree(all_blocks_addr);
    return -1; // 遍历完整个目录都没找到有效条目
}

// 由于现在我们的 i_size 字段只增不减了，因此我们不能在用老方法来判断文件夹是否为空了
// 必须暴力扫描
bool sifs_dir_is_empty(struct inode* dir_inode) {
    // 构造一个临时 file，用于记录扫描进度，不影响任何人的 fd_pos
    struct file temp_f;
    temp_f.fd_inode = dir_inode;
    temp_f.fd_pos = 0; // 强制从头开始扫

    struct dirent de;
    // sifs_dir_read 返回 0 表示读到一个有效条目
    while (sifs_dir_read(&temp_f, &de) == 0) {
        // 过滤掉每个目录都有的两个固定成员
        if (strcmp(de.d_name, ".") != 0 && strcmp(de.d_name, "..") != 0) {
            // 只要发现一个不是 . 也不是 .. 的有效条目，目录就不为空
            return false;
        }
        // 继续下一次读取...
    }

    // 如果循环正常结束（返回 -1），说明扫完了 i_size 也没发现有效项，可以返回空
    return true;
}

// 物理删除一个子目录，主要由 rmdir 使用
int32_t sifs_dir_remove(struct inode* parent_inode, struct inode* child_inode) {
    struct partition* part = get_part_by_rdev(parent_inode->i_dev);

    // 函数判断目录是否为空
    // 如果 i_size > 2个目录项的大小，说明肯定有文件
    if (!sifs_dir_is_empty(child_inode)) {
        printk("sifs_dir_remove: directory is not empty, cannot remove!\n");
        return -1; 
    }

    // 检查高位块是否确实为 0
    // 虽然 i_size 检查已经涵盖了大部分情况，但检查 sectors 指针主要是为了防止文件系统损坏
    for (int block_idx = 1; block_idx < 13; block_idx++) {
        if (child_inode->sifs_i.i_sectors[block_idx] != 0) {
             printk("sifs_dir_remove: unexpected allocated blocks in empty directory!\n");
             return -1;
        }
    }

    void* io_buf = kmalloc(SECTOR_SIZE * 2);
    if (io_buf == NULL) {
        printk("sifs_dir_remove: malloc for io_buf failed!\n");
        return -1;
    }

    // 在父目录中抹除该子目录的记录
    if (!sifs_delete_dir_entry(part, parent_inode, child_inode->i_no, io_buf)) {
        kfree(io_buf);
        return -1;
    }

    // 彻底回收子目录 Inode 及它占用的第 0 个数据块
    // inode_release 内部会根据 i_sectors 回收所有已分配的数据块位图
    inode_release(part, child_inode->i_no);

    kfree(io_buf);
    return 0;
}