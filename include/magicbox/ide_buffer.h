#ifndef __DEVICE_BUFFER_H
#define __DEVICE_BUFFER_H

#include "stdint.h"
#include "stdbool.h"
#include "unistd.h"
#include "dlist.h"
#include "sync.h"
#include "hashtable.h"

struct disk;

#define BUFFER_NUM 2048
// 由于要使用黄金分割乘法hash，因此选取2的幂次作为hash_size
// 我们的malloc函数，当申请内存大于1024B时，会直接分配一个页的内存给该进程
// 哈希表中会有一个dlist数组，每一个dlist元素的大小是16字节
// 128*16=2KB，刚好占用一个页的一半，我们留出另一半来作为一点余裕
// 防止后面出现莫名其妙的问题
#define HASH_SIZE 128  
#define WRITE_BATCH_SIZE SECTORS_PER_OP_BLOCK // 用于在bread_multi进行批量写入的控制
struct buffer_head {
    uint32_t b_blocknr;     // 对应的磁盘 LBA 地址（扇区号）
    struct disk* b_dev;     // 属于哪个磁盘设备

    bool b_dirty;           // 脏位：内存已被修改，尚未同步到磁盘
    bool b_valid;           // 有效位：内存数据是否已从磁盘读入（如果是空的则为false）
    uint32_t b_ref_count;   // 引用计数：有多少个进程正在使用这个块（防止被意外回收）

    uint8_t *b_data;           // 指向 512 字节内存空间的指针

    // 我们希望每一次对元素在LRU链表中的操作都不要影响到其在hash表中的位置
    // 因此我们需要将hash_tag和lru_tag分开
    struct dlist_elem lru_tag; // 哈希表节点：用于根据 (dev, lba) 快速找到块
    struct dlist_elem hash_tag;  // LRU节点：用于当缓冲区满时，决定踢掉哪个“最老”的块
};

struct ide_buffer {
    struct lock lock;                // 覆盖整个buffer的锁
    struct buffer_head* cache_pool;  // 缓存池
    struct hashtable hash_table;     // hash表，用于快速查询和索引
    struct dlist lru_list;           // LRU 队列（其实就是一个 dlist）
};

extern void ide_buffer_init(void);
extern struct buffer_head* bread(struct disk* dev,uint32_t lba);
extern void bwrite(struct disk* dev, uint32_t lba, void* src_buf);
extern void brelse(struct buffer_head* bh);
extern void bread_multi(struct disk* dev, uint32_t start_lba,void* out_buf , uint32_t sec_cnt);
extern void bwrite_multi(struct disk* dev, uint32_t start_lba, void* src_buf, uint32_t sec_cnt);
#endif