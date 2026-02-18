#include "ide_buffer.h"
#include "ide.h"
#include "stdint.h"
#include "stdbool.h"
#include "stdio-kernel.h"
#include "debug.h"
#include "sync.h"
#include "hashtable.h"
#include "interrupt.h"

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
    global_ide_buffer.cache_pool = (struct buffer_head*)kmalloc(sizeof(struct buffer_head)*BUFFER_NUM);
    if(NULL == global_ide_buffer.cache_pool){
        PANIC("fail to allocate ide.cache_pool!\n");
    }
    dlist_init(&global_ide_buffer.lru_list);

    hash_init(&global_ide_buffer.hash_table,HASH_SIZE,buffer_hash,buffer_condition);
    
    // 将cachepool中的元素插入lruqueue中
    int pool_elem_idx = 0;

    struct buffer_head* cache_pool = global_ide_buffer.cache_pool;

    for(pool_elem_idx = 0;pool_elem_idx<BUFFER_NUM;pool_elem_idx++){
        dlist_push_back(&global_ide_buffer.lru_list,&(cache_pool[pool_elem_idx].lru_tag));
        cache_pool[pool_elem_idx].b_data = kmalloc(SECTOR_SIZE); 
        if (cache_pool[pool_elem_idx].b_data == NULL) PANIC("Buffer data allocation failed!");

        cache_pool[pool_elem_idx].b_blocknr = 0xffffffff; // 初始化为无效块号,
        cache_pool[pool_elem_idx].b_dev = NULL; // 初始化为无效设备
        cache_pool[pool_elem_idx].b_dirty = false;
        cache_pool[pool_elem_idx].b_valid = false;
        cache_pool[pool_elem_idx].b_ref_count = 0;
    }
    // struct dlist_elem* elem = global_ide_buffer.lru_list.head.next;
    // for(pool_elem_idx = 0;pool_elem_idx<BUFFER_NUM;pool_elem_idx++){
    //     printk("b_blocknr: %d\n",(member_to_entry(struct buffer_head,lru_tag,elem))->b_blocknr);
    //     elem = elem->next;
    // }
    lock_release(&global_ide_buffer.lock);
    printk("ide_buffer_init done\n");
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

    // 未命中，从缓冲区中空闲的缓存块
    // 我们优先选取那些b_valid为false的块
    // 如果实在没有再选取那些b_ref_count为0的块
    struct dlist_elem *pelem = global_ide_buffer.lru_list.head.next;
    struct buffer_head* tmp_bh = NULL;
    while(pelem != &global_ide_buffer.lru_list.tail){
        tmp_bh = member_to_entry(struct buffer_head,lru_tag,pelem);
        if(0==tmp_bh->b_ref_count){
            // 如果没有b_valid为false的块，那就取b_ref_count为0的块
            // b_valid为true的块一定在b_valid为false的块后面
            // 因为b_valid为true说明它曾被引用过，在那时它一定会被放到lru队列的尾部！
            // 因此，如果有b_valid为false的块的话，我们从前往后扫一定会优先取到它
            // 而不是b_valid为true但b_ref_count为0的块
            // 因此只需要一个条件判断即可
            bh=tmp_bh;
            break;
        }
        pelem=pelem->next;
    }
    // 如果缓冲区中没有引用计数为0的块
    // 说明缓冲区已满，先PANIC，后续可以为缓冲区加一个等待队列
    // 如果满的话先将这个进程挂在等待队列上
    if(NULL == bh){
        lock_release(&global_ide_buffer.lock);
        PANIC("getblk: no free buffer available! Memory pressure too high.");
    }

    // 如果这个块以前被其他进程用过
    // 那么此时它还在hash表中
    // 因为我们在缓冲区中对hash表的释放操作是惰性释放的
    // 这样可以使得一个块虽然ref计数为0，但是仍在hash表中
    // 若紧接着它又被命中了，我们就可以直接使用它了，无需再重新创建了
    // 但是也正因为如此，我们在这里需要对这样的缓冲块进行一个额外处理
    // 先将其从hash表中移除出来，然后再重新为其在hash表中分配新位置
    if(false != bh->b_valid){
        hash_remove_elem(&global_ide_buffer.hash_table,&bh->hash_tag);
    }
    bh->b_blocknr = lba;
    bh->b_dev = dev; 
    bh->b_ref_count = 1;
    bh->b_valid = false; // 新块的b_valid在bread前一定要是false的！
    bh->b_dirty = false;
    hash_insert(&global_ide_buffer.hash_table,(void*)(&bk),&bh->hash_tag);

    // 更新lru，将当前块移动到队尾
    dlist_remove(&bh->lru_tag);
    dlist_push_back(&global_ide_buffer.lru_list,&bh->lru_tag);
    lock_release(&global_ide_buffer.lock);
    return bh;
}

struct buffer_head* bread(struct disk* dev,uint32_t lba){
    // 由于 getblk 已经持锁保证了同一个 LBA 只会对应同一个 bh 实例
    // 即便发生了重复读取，也只是浪费了一点 I/O 时间
    // 不会导致内存数据不一致。
    // 因此此处不用额外上锁了
   
    // 先获取缓冲区句柄
    struct buffer_head * bh = getblk(dev,lba);
    // 如果 b_valid 为 false，说明此次缓存没命中
    // 进行真正的io
    if(false == bh->b_valid){
        // 调用底层的磁盘驱动读接口
        ide_read(dev, lba, bh->b_data, 1);
        // 读完后，将该块标记为有效
        bh->b_valid = true;
    }
    
    // 如果valid为true，那么说明此次命中了，直接返回，不io
    // 其实这也是我们为什么要在getblk的最后
    // 将未命中的块的 bh->b_valid 置为false的原因
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

    uint8_t* dst = (uint8_t*)out_buf;
    uint32_t i = 0;

    while (i < sec_cnt) {
        uint32_t curr_lba = start_lba + i;
        struct buffer_key bk = {curr_lba, dev};

        // 先尝试在哈希表中查找（必须持有全局锁）
        lock_acquire(&global_ide_buffer.lock);
        struct dlist_elem* de = hash_find(&global_ide_buffer.hash_table, (void*)(&bk));
        
        if (de != NULL) {
            // 命中缓存，直接从内存拷贝
            struct buffer_head* bh = member_to_entry(struct buffer_head, hash_tag, de);
            bh->b_ref_count++; 
            lock_release(&global_ide_buffer.lock);

            memcpy(dst + (i * SECTOR_SIZE), bh->b_data, SECTOR_SIZE);
            
            brelse(bh); // 释放引用
            i++;
        }else{
            // 未命中，释放锁，准备进入批量读取模式
            lock_release(&global_ide_buffer.lock);

            uint32_t bulk_start_idx = i;
            // 限制单次批量大小，16个扇区(8KB)是一个比较平衡的数值
            uint32_t bulk_cnt = (sec_cnt - i > SECTORS_PER_OP_BLOCK) ? SECTORS_PER_OP_BLOCK : (sec_cnt - i);
            uint32_t j =0 ;

            // 向后探测：如果在 bulk 范围内遇到了已经缓存的块，就截断本次批量读取
            for (j = 1; j < bulk_cnt; j++) {
                struct buffer_key tmp_bk = {start_lba + bulk_start_idx + j, dev};
                lock_acquire(&global_ide_buffer.lock);
                if (hash_find(&global_ide_buffer.hash_table, (void*)(&tmp_bk))) {
                    lock_release(&global_ide_buffer.lock);
                    bulk_cnt = j; // 缩减本次读取范围，避免覆盖已有的缓存数据
                    break;
                }
                lock_release(&global_ide_buffer.lock);
            }

            // 快速读取，直接读入用户提供的 dst ---
            // 这一步保证了第一次执行的速度
            ide_read(dev, start_lba + bulk_start_idx, dst + (bulk_start_idx * SECTOR_SIZE), bulk_cnt);

            // 缓存同步：将读到的数据登记到 Buffer Cache
            // 这一步保证了第二次执行的速度
            
            for (j = 0; j < bulk_cnt; j++) {
                uint32_t lba = start_lba + bulk_start_idx + j;
                
                // getblk 会在哈希表中创建新项，并从 LRU 头部拿走一个空闲块
                struct buffer_head* bh = getblk(dev, lba);
                
                // 将刚才读到 dst 里的数据同步一份到缓存块中
                memcpy(bh->b_data, dst + ((bulk_start_idx + j) * SECTOR_SIZE), SECTOR_SIZE);
                
                bh->b_valid = true;  // 标记为有效
                bh->b_dirty = false;
                // 释放并放回 LRU 队尾
                // 此处最好要进行这个释放操作，这是“借阅-归还”机制，用完立马还
                // 因为如果不释放的话，当我们进行大批量的数据读取时，缓冲区会被很快耗尽
                // 由于我们是惰性释放，因此如果下一次还用到同一个块，它还是能命中
                // 并且如果下一次没有其他的b_valid=false块可以用了
                // 那么我们刚刚归还回去的块就可以被其他的进程使用
                brelse(bh); 
            }

            i += bulk_cnt;
        }
    }
}

// void ide_write(struct disk* hd,uint32_t lba,void* buf,uint32_t sec_cnt){

// 采用直写式缓存
void bwrite(struct disk* dev, uint32_t lba, void* src_buf) {
    // 获取并锁定缓存块 (ref_count++)
    struct buffer_head* bh = getblk(dev, lba);
    
    // 更新缓存内容，否则写磁盘也是写旧数据
    memcpy(bh->b_data, src_buf, SECTOR_SIZE);
    bh->b_valid = true; 

    // 标记并同步物理磁盘
    bh->b_dirty = true; 
    ide_write(bh->b_dev, bh->b_blocknr, bh->b_data, 1);
    bh->b_dirty = false;

    // 释放引用 (ref_count--)，否则下次 getblk 会 PANIC
    brelse(bh);
}


void bwrite_multi(struct disk* dev, uint32_t start_lba, void* src_buf, uint32_t sec_cnt) {

    if(sec_cnt==1){
        bwrite(dev,start_lba,src_buf);
        return;
    }

    uint32_t secs_done = 0;
    
    // 定义批处理上限，防止bh_list过大耗尽栈空间，32个指针仅占用 128 字节栈空间，比较安全
    
    struct buffer_head* bh_list[WRITE_BATCH_SIZE]; 

    while (secs_done < sec_cnt) {
        // 计算本次批处理的扇区数
        uint32_t curr_batch_cnt = (sec_cnt - secs_done > WRITE_BATCH_SIZE) ? 
                                   WRITE_BATCH_SIZE : (sec_cnt - secs_done);
        
        uint32_t i;
        uint8_t* batch_src = (uint8_t*)src_buf + (secs_done * SECTOR_SIZE);

        // 内存同步：获取并锁定本批次的 Buffer Cache
        for (i = 0; i < curr_batch_cnt; i++) {
            uint32_t current_lba = start_lba + secs_done + i;
            
            // getblk 会增加 ref_count，锁定该块不被置换
            bh_list[i] = getblk(dev, current_lba);
            
            // 更新缓存内容
            memcpy(bh_list[i]->b_data, batch_src + (i * SECTOR_SIZE), SECTOR_SIZE);
            
            // 标记为有效
            bh_list[i]->b_valid = true;
        }

        // 批量物理落盘：此时 bh_list 中的块全部被锁定，数据一致性得到保证
        // ide_write 内部会利用 CMD_WRITE_MULTIPLE 快速写入
        ide_write(dev, start_lba + secs_done, batch_src, curr_batch_cnt);

        // 释放本批次的引用计数，同样是检阅-归还机制
        for (i = 0; i < curr_batch_cnt; i++) {
            brelse(bh_list[i]);
        }

        secs_done += curr_batch_cnt;
    }
}