#include <ide_buffer.h>
#include <ide.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio-kernel.h>
#include <debug.h>
#include <sync.h>
#include <hashtable.h>
#include <interrupt.h>

static struct ide_buffer global_ide_buffer; 

// 使用磁盘和lba可以唯一确定一个块
// 由于每一个磁盘都会在内存中分配一个disk镜像
// 因此每一个磁盘数据结构的地址具有唯一性
// 我们可以用它来进行哈希。
struct buffer_key{
    uint32_t lba;
    struct disk* disk;
};

// 缓冲区buffer计算函数
static uint32_t buffer_hash(void* arg) {
    struct buffer_key* key = (struct buffer_key*)arg;
    
    // 混合 disk 指针和 lba 地址
    // 指针右移 4 位是因为对象通常按 16 字节对齐，低位信息量少
    uint32_t val = ((uint32_t)key->disk >> 4) ^ key->lba;
    
    // 黄金分割乘法: 先打散数据
    return val * HASH_GOLDEN_RATIO_32;
}

static bool buffer_condition(struct dlist_elem* pelem,void* arg){
    struct buffer_key* bk = (struct buffer_key*)arg;
    struct buffer_head* bh = member_to_entry(struct buffer_head,hash_tag,pelem);
    return bh->b_dev == bk->disk&&bh->b_blocknr == bk->lba;
}


void ide_buffer_init(){
    printk("ide_buffer_init...\n");

    
    lock_init(&global_ide_buffer.lock);
    lock_acquire(&global_ide_buffer.lock);
    global_ide_buffer.max_blk_num = BUFFER_NUM; 
    global_ide_buffer.cur_blk_num = 0;
    global_ide_buffer.buf_blk_size = SECTOR_SIZE; 
    dlist_init(&global_ide_buffer.lru_list);

    hash_init(&global_ide_buffer.hash_table,HASH_SIZE,buffer_hash,buffer_condition);
    
    lock_release(&global_ide_buffer.lock);
    printk("ide_buffer_init done\n");
}

// 从缓存中驱逐一个缓存块
static bool blk_evict(void) {
    // 先从 LRU 中找到一个可用的缓存块
    struct dlist_elem* pelem = global_ide_buffer.lru_list.head.next;
    struct buffer_head* victim = NULL;

    while (pelem != &global_ide_buffer.lru_list.tail) {
        struct buffer_head* tmp = member_to_entry(struct buffer_head, lru_tag, pelem);
        
        // 只有没有进程使用的块才可以被驱逐
        if (tmp->b_ref_count == 0) {
            victim = tmp;
            break; 
        }
        pelem = pelem->next;
    }

    if (!victim) {
        // 如果翻遍了 LRU 都没有 ref_count == 0 的块，
        // 说明此时系统压力过大，所有缓存都在被并发使用。
        // 这时可以考虑暂时突破 MAX_BUFFER_NUM，或者 PANIC。
        PANIC("blk_evict: buffer exhausted!\n");
        return false;
    } 

    // 安全检查
    // 理论上被选为牺牲者的块 ref_count 必须为 0
    ASSERT(victim->b_ref_count == 0);


    // 我们先采用直写缓存，因此此处直接淘汰就行

    // 脏数据落盘 (Write-back)
    // 如果该块在内存中被修改过，必须先同步到磁盘
    // if (victim->b_dirty) {
    //     // 这里的 ide_write 是阻塞的
    //     ide_write(victim->b_dev, victim->b_blocknr, victim->b_data, 1);
    //     victim->b_dirty = false;
    // }

    

    // 从追踪结构中摘除
    // 这样之后 getblk 就彻底找不到这个块了
    hash_remove(&global_ide_buffer.hash_table, &victim->hash_tag);
    dlist_remove(&victim->lru_tag);

    // 彻底销毁内存对象
    // 先释放数据区，再释放管理结构
    kfree(victim->b_data);
    kfree(victim);

    // 更新全局统计计数
    global_ide_buffer.cur_blk_num--;
    return true;
}

static struct buffer_head* getblk(struct disk* dev, uint32_t lba){
    // 首先现在hash表中查看该内存是否缓存命中
    struct buffer_key bk= {lba,dev};
    struct dlist_elem* de = NULL;
    struct buffer_head* bh = NULL;

    lock_acquire(&global_ide_buffer.lock);
    de = hash_find(&global_ide_buffer.hash_table,(void*)(&bk));
    // 命中
    if(NULL!=de){
        bh = member_to_entry(struct buffer_head,hash_tag,de);
        // 添加引用计数，将元素移动到lru队列队尾
        bh->b_ref_count++;
        dlist_remove(&bh->lru_tag);
        dlist_push_back(&global_ide_buffer.lru_list,&bh->lru_tag);
        lock_release(&global_ide_buffer.lock);
        return bh;
    }

    // 未命中，先检查是否达到了阈值
    // 如果达到了阈值，那么就执行 evict 逻辑 kfree 释放一下不需要的块再 kmalloc
    // 最好用 while，因为一次 evict 可能由于某些块被锁定而失败，或者需要腾出更多空间
    while (global_ide_buffer.cur_blk_num >= global_ide_buffer.max_blk_num) {
        // blk_evict 成功驱逐返回 true，无块可驱逐返回 false
        if (!blk_evict()) break; 
    }

    // 动态申请
    // 申请管理结构
    bh = (struct buffer_head*)kmalloc(sizeof(struct buffer_head));

    if(bh == NULL){
        // 此处先直接 panic 简单处理
        // 正常情况下，我们可以在此处先执行一次 evict，腾出些内存再尝试重新 malloc
        PANIC("getblk: bh is NULL!");
    }

    // 申请数据区
    bh->b_data = kmalloc(global_ide_buffer.buf_blk_size); 

    if(bh->b_data == NULL){
        // 此处先直接 panic 简单处理
        // 正常情况下，我们可以在此处先执行一次 evict，腾出些内存再尝试重新 malloc
        PANIC("getblk: bh->b_data is NULL!");
    }

    // 初始化属性并挂载
    bh->b_dev = dev;
    bh->b_blocknr = lba;
    bh->b_ref_count = 1;
    bh->b_dirty = false;
    bh->b_valid = false; // 由于没有存有真实的数据，是新申请的，所以没有有效数据，valid为false
    hash_insert(&global_ide_buffer.hash_table,(void*)(&bk),&bh->hash_tag);
    dlist_push_back(&global_ide_buffer.lru_list,&bh->lru_tag);
    global_ide_buffer.cur_blk_num++;

    lock_release(&global_ide_buffer.lock);
    
    return bh;
}

void brelse(struct buffer_head* bh){
    if(NULL==bh){
        return;
    }

    lock_acquire(&global_ide_buffer.lock);
    
    if(bh->b_ref_count > 0){
        bh->b_ref_count--;
    }else{
        // 如果计数已经是0了还在释放，说明上层逻辑有严重Bug
        PANIC("brelse: buffer_head ref_count is already 0!\n");
    }
    // 此时我们不移动 LRU 链表，也不移除 Hash。
    // 因为在 getblk 中，命中时会把块移到队尾，
    // 而没命中时会从队首寻找 ref_count == 0 的块。
    // 这样自然实现了：经常被 bread/brelse 的块留在队尾，
    // 而释放后长期没人碰的块会慢慢“沉降”到队首被回收。
    lock_release(&global_ide_buffer.lock);
}

void bread_multi(struct disk* dev, uint32_t start_lba, void* out_buf, uint32_t sec_cnt) {
    uint32_t i = 0;
    while (i < sec_cnt) {
        // 先尝试在哈希表中查找（必须持有全局锁）
        struct buffer_head* bh = getblk(dev, start_lba + i);
        
        if (bh->b_valid) { 
            // bh->b_valid 为 true 说明是缓存命中的，不是新申请的，可以直接使用，不用 io
            memcpy((uint8_t*)out_buf + i * SECTOR_SIZE, bh->b_data, SECTOR_SIZE);
            // getblk 中会增加计数，所以此处我们用完后需要释放一下
            brelse(bh);
            i++;
        } else {
            // bh->b_valid 为 false 说明这是一个新申请的 bh，没有有效数据，需要发起磁盘 io
            // 尝试向后合并 IO
            // 至少我们当前的这个块肯定是没命中的，因此 bulk_cnt 初始为 1
            uint32_t bulk_cnt = 1; 
            // 释放掉这个还没填数据的 bh，我们一会儿批量处理它
            brelse(bh); 

            // 向后看还有多少个块也是不命中的，直到遇到命中的或者到结尾
            lock_acquire(&global_ide_buffer.lock);
            while (i + bulk_cnt < sec_cnt) {
                struct buffer_key bk = {start_lba + i + bulk_cnt, dev};
                
                struct dlist_elem* bh_hash_tag = hash_find(&global_ide_buffer.hash_table,(void*)&bk);
                // 如果哈希表里找不到，说明可以合并读取
                if(!bh_hash_tag){
                    bulk_cnt++;
                } else {
                    // 如果找到了，说明内存里已经有这个块了，停止合并
                    // 下一轮循环时，进入 while 后 getblk 得到的 bh 的 valid 就为 true 了
                    break;
                }
                // 防止单次 IO 太大
                // 我们设置的最大一次性 io 的扇区数是 16，因此我们将其作为上界
                if (bulk_cnt >= SECTORS_PER_OP_BLOCK) break; 
            }
            lock_release(&global_ide_buffer.lock);

            // 发起一次大批量 IO (读到用户缓冲区)
            ide_read(dev, start_lba + i, (uint8_t*)out_buf + i * SECTOR_SIZE, bulk_cnt);

            // 既然读完了，顺便把这些块同步进缓存
            for (uint32_t j = 0; j < bulk_cnt; j++) {
                struct buffer_head* tmp_bh = getblk(dev, start_lba + i + j);
                // 只有拿到的是无效块时才拷贝（防止在 ide_read 期间别的进程填了数据）
                if (!tmp_bh->b_valid) {
                    memcpy(tmp_bh->b_data, (uint8_t*)out_buf + (i + j) * SECTOR_SIZE, SECTOR_SIZE);
                    tmp_bh->b_valid = true;
                }
                brelse(tmp_bh);
            }
            i += bulk_cnt;
        }
    }
}

// void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt)
// 采用直写式缓存
void bwrite_multi(struct disk* dev, uint32_t start_lba, void* src_buf, uint32_t sec_cnt) {
    uint32_t secs_done = 0;

    while (secs_done < sec_cnt) {
        // 我们最多一次性只能连续写回 16 块，因此先截断一下
        uint32_t curr_batch_cnt = (sec_cnt - secs_done > SECTORS_PER_OP_BLOCK) ? 
                                   SECTORS_PER_OP_BLOCK : (sec_cnt - secs_done);
        
        uint8_t* batch_src = (uint8_t*)src_buf + (secs_done * SECTOR_SIZE);
        uint32_t current_lba_base = start_lba + secs_done;

        // 物理落盘优先，直接把用户数据写进去，这是最快的
        // 这一步保证了至少磁盘数据永远是最新的
        ide_write(dev, current_lba_base, batch_src, curr_batch_cnt);
        // 直写式缓存同步，只处理那些“已经在缓存里”的块 以便保证缓存一致
        // 如果每一次写操作都将所有数据都更新进缓存的话，设想一种这样的情况
        // 如果我们的缓存只有 4MB，用户这时写了 10MB 的数据到磁盘中
        // 如果这 10MB 全都写到缓存里的话，我们的缓存会迅速耗尽！
        lock_acquire(&global_ide_buffer.lock);
        for (uint32_t i = 0; i < curr_batch_cnt; i++) {
            uint32_t current_lba = current_lba_base + i;
            
            // 这里不要用 getblk，因为 getblk 没命中会新建。
            // 我们需要“只查不增”
            struct buffer_key bk = {current_lba, dev};
            
            struct dlist_elem* de = hash_find(&global_ide_buffer.hash_table, &bk);
            
            if (de) {
                struct buffer_head* bh = member_to_entry(struct buffer_head, hash_tag, de);
                // 既然磁盘已经写成功了，如果缓存里有，顺手更新掉
                memcpy(bh->b_data, batch_src + (i * SECTOR_SIZE), SECTOR_SIZE);
                bh->b_valid = true; 
                bh->b_dirty = false; // 因为已经直写落盘了，所以它是干净的
            }
            
        }
        lock_release(&global_ide_buffer.lock);

        secs_done += curr_batch_cnt;
    }
}