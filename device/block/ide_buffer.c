#include <ide_buffer.h>
#include <ide.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio-kernel.h>
#include <debug.h>
#include <sync.h>
#include <hashtable.h>
#include <interrupt.h>
#include <memory.h>
#include <timer.h>
#include <thread.h>

// 单次合并写入的最大扇区数
#define MAX_SYNC_COUNT SECTORS_PER_OP_BLOCK

extern struct task_struct* sync_thread;

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

// arg 传入的是待插入的 pelem 指针本身
static bool _cb_bh_lba_condition(struct dlist_elem* pelem, void* arg) {
    struct dlist_elem* new_elem = (struct dlist_elem*)arg;
    
    // 从 tag 还原回 buffer_head
    struct buffer_head* bh_in_list = member_to_entry(struct buffer_head, dirty_tag, pelem);
    struct buffer_head* bh_new     = member_to_entry(struct buffer_head, dirty_tag, new_elem);
    
    // 如果当前链表中的块 LBA 大于新块，说明找到了插入位置
    return bh_in_list->b_blocknr > bh_new->b_blocknr;
}

void ide_buffer_init(){
    printk("ide_buffer_init...\n");

    
    lock_init(&global_ide_buffer.lock);
    lock_acquire(&global_ide_buffer.lock);
    global_ide_buffer.buf_blk_size = SECTOR_SIZE; 
    global_ide_buffer.max_blk_num = (mem_bytes_total / BUFFER_RATE)/global_ide_buffer.buf_blk_size; 
    global_ide_buffer.cur_blk_num = 0;
    
    dlist_init(&global_ide_buffer.lru_list);

    hash_init(&global_ide_buffer.hash_table,HASH_SIZE,buffer_hash,buffer_condition);
    
    lock_release(&global_ide_buffer.lock);
    printk("max buffer num: %d\n",global_ide_buffer.max_blk_num);
    printk("ide_buffer_init done\n");
}

// 从 LRU 中找到一个可用的缓存块
static struct buffer_head* find_victim(void){
    
    struct dlist_elem* pelem = global_ide_buffer.lru_list.head.next;
    struct buffer_head* victim = NULL;

    while (pelem != &global_ide_buffer.lru_list.tail) {
        struct buffer_head* tmp = member_to_entry(struct buffer_head, lru_tag, pelem);
        
        // 只有没有进程使用的块才可以被驱逐
        // 只要有引用，不管脏不脏，无论如何都不能释放
        if (tmp->b_ref_count == 0) {
            victim = tmp;
            break; 
        }
        pelem = pelem->next;
    }

    return victim;
}

// 从缓存中驱逐一个缓存块
static bool blk_evict(struct buffer_head* victim) {

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

    // 我们在函数外部进行了写回淘汰，因此此处直接淘汰就行
    // 从追踪结构中摘除
    // 这样之后 getblk 就彻底找不到这个块了
    hash_remove(&global_ide_buffer.hash_table, &victim->hash_tag);
    dlist_remove(&victim->lru_tag);

    // 一定要及时把所有的 tag 都移除相应的队列
    if(dlist_is_linked(&victim->dirty_tag)){
        dlist_remove(&victim->dirty_tag);
    }

    // 彻底销毁内存对象
    // 先释放数据区，再释放管理结构
    kfree(victim->b_data);
    kfree(victim);

    // 更新全局统计计数
    global_ide_buffer.cur_blk_num--;
    return true;
}

static struct buffer_head* getblk(struct disk* dev, uint32_t lba) {
    struct buffer_key bk = {lba, dev};
    
    // 缓存命中
    lock_acquire(&global_ide_buffer.lock);
    struct dlist_elem* de = hash_find(&global_ide_buffer.hash_table, &bk);
    if (de) {
        struct buffer_head* bh = member_to_entry(struct buffer_head, hash_tag, de);
        bh->b_ref_count++;
        dlist_remove(&bh->lru_tag);
        dlist_push_back(&global_ide_buffer.lru_list, &bh->lru_tag);
        lock_release(&global_ide_buffer.lock);
        return bh;
    }

    // 压力预警，当负载达到 80% 了，进行后台刷脏，但是不阻塞当前进程 
    if (global_ide_buffer.cur_blk_num > (global_ide_buffer.max_blk_num * 8 / 10)) {
        // 唤醒后台 sync 线程
        if(sync_thread->status == TASK_WAITING){
            thread_unblock(sync_thread);
        }
    }

    // 慢速路径 (回收与分配) 
    // 如果缓存达到了 90% 满了，则直接在当前进程中阻塞回收
    // 我们在第二次拿锁插入时，并没有检查 cur_blk_num
    // 虽然我们已经执行了 blk_evict，但在释放锁去 kmalloc 的间隙
    // 如果有多个进程同时并发地执行 getblk 分配不同的块，它们可能会同时穿过 while 循环，然后各自申请内存
    // 最后在插入阶段依次增加 cur_blk_num。
    // 这会导致 cur_blk_num 暂时性地超过 max_blk_num
    // 为了减少这种情况出现的概率，我们的阻塞回收阈值设置在 90% 
    // 这样的话不容易超过最大限制，即使超过了其实也没事，因为我们下面的 blk_evict 是用 while 执行的
    // 那些超过的部分都会被刷走
    while (global_ide_buffer.cur_blk_num >= (global_ide_buffer.max_blk_num * 9 / 10)) {
        struct buffer_head* victim = find_victim();
        if (!victim) {
            // 若全员处于引用中
            // 可以选择 sleep 等待别人 brelse
            // 或者暂时允许超过 max 限制（动态伸缩）
            // 但是目前我们先 panic 来组织这种情况
            PANIC("blk_evict: buffer exhausted!\n");
            break; 
        }

        // 如果 victim 是脏的，那么我们得给他写回后再淘汰它
        if (victim->b_dirty) {
            // 必须先摘除节点，防止 sync_thread 碰到它，出现竞态问题
            // 因为 sync_thread 也会进行脏块的刷盘操作
            // 此处对一个脏块进行刷盘后，如果不及时将其移出脏队列的话
            // 待会儿 sync_thread 又会对这个脏块进行 sync
            // 但是此处的 blk_evict 已经将相应的脏块给 kfree 了
            // blk_evict 里面不会将相应的脏块移出 dirty 队列，只会将其移出 lru 队列和 hash 表
            // 因此待会儿 sync_thread 又会去访问这个空的数据块！
            // 出现稀奇古怪的问题
            lock_acquire(&victim->b_dev->lists_lock);
            dlist_remove(&victim->dirty_tag); // 彻底从脏链表剥离
            lock_release(&victim->b_dev->lists_lock);

            // 在 IO 前释放全局锁，否则整个系统缓存都会卡死在磁盘 IO 上
            lock_release(&global_ide_buffer.lock);
            ide_write(victim->b_dev, victim->b_blocknr, victim->b_data, 1);
            lock_acquire(&global_ide_buffer.lock);
            
            victim->b_dirty = false;
            // 此时它已经不在任何链表了，可以直接 blk_evict
            blk_evict(victim); 
            continue;
        }

        blk_evict(victim);
    }
    lock_release(&global_ide_buffer.lock);

    // 在锁外申请内存，降低锁竞争
    struct buffer_head* new_bh = kmalloc(sizeof(struct buffer_head));
    new_bh->b_data = kmalloc(global_ide_buffer.buf_blk_size);
    if(new_bh == NULL || new_bh->b_data == NULL){
        // 此处先直接 panic 简单处理
        // 正常情况下，我们可以在此处先执行一次 evict，腾出些内存再尝试重新 malloc
        PANIC("getblk: fail to kmalloc!");
    }

    // 插入 (Double Check) 
    lock_acquire(&global_ide_buffer.lock);

    de = hash_find(&global_ide_buffer.hash_table, &bk);
    if (de) {
        // 别人抢先创建了，自杀并返回现有的
        kfree(new_bh->b_data); kfree(new_bh);
        struct buffer_head* bh = member_to_entry(struct buffer_head, hash_tag, de);
        bh->b_ref_count++;
        lock_release(&global_ide_buffer.lock);
        return bh;
    }
    
    // 初始化属性并挂载
    new_bh->b_dev = dev;
    new_bh->b_blocknr = lba;
    new_bh->b_ref_count = 1;
    new_bh->b_dirty = false;
    new_bh->b_valid = false; // 由于没有存有真实的数据，是新申请的，所以没有有效数据，valid为false
    hash_insert(&global_ide_buffer.hash_table,(void*)(&bk),&new_bh->hash_tag);
    dlist_push_back(&global_ide_buffer.lru_list,&new_bh->lru_tag);
    global_ide_buffer.cur_blk_num++;

    lock_release(&global_ide_buffer.lock);

    return new_bh;
}

void brelse(struct buffer_head* bh){
    // printk("brelse: 0x%x refcnt: %d\n",bh->b_blocknr,bh->b_ref_count);
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

// bread 的零拷贝版本，用户不需要传入缓冲区，而是直接操作缓冲区中的内存副本
// 此处的 lba 需要使用绝对的 lba 地址，不能使用相对 lba 地址，所以此函数叫做 _bread
// 我们正常使用需要使用宏 bread，它会负责将相对 lba 转成绝对 lba 地址
// 由于 _bread 直接引用了 ide 缓存，因此用户需要自己管理 缓存计数，使用完毕后需要手动 brelse
// 目前最主要就是给 ext2_write_inode 和 ext2_read_inode 来用，因为他们会被调用用来读取 inode，并且一般只读一个扇区，使用该函数可以提速
// 我们目前的延迟写回中， inode 的缓存仍然是直写缓存，每次操作进行完毕后都直接调用 write_inode 写回
// 将延迟写回的任务全部放到 ide 层上来做
struct buffer_head* _bread(struct disk* dev, uint32_t lba) {
    // 获取块，getblk 内部处理了引用计数 ref_count++
    struct buffer_head* bh = getblk(dev, lba);
    
    // 如果数据无效，直接读盘到缓存区
    if (!bh->b_valid) {
        // getblk 已经拿到了全局锁，但 ide_read 是阻塞且开中断的
        // 此处可能会引发进程切换
        ide_read(dev, lba, bh->b_data, 1);
        bh->b_valid = true; // 当前的数据就是最新数据，valid 置为 true
        bh->b_dirty = false; 
    }
    
    // 返回结构体指针，用户通过 bh->b_data 访问数据
    return bh;
}

// 由于多扇区的读写会涉及到多个 buffer_head，目前的架构当中不太好针对这种情况实现零拷贝
// 因此多扇区读的部分还是依然先使用有拷贝的实现
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

void sync_ide_buffer(void *arg UNUSED) {
    // 预分配一个足够大的临时缓冲区，避免在循环里频繁申请内存
    // 大小为 SECTORS_PER_OP_BLOCK * 512
    uint32_t io_buffer_sec_cnt = (MAX_SYNC_COUNT>global_ide_buffer.max_blk_num)?(global_ide_buffer.max_blk_num):MAX_SYNC_COUNT;
    uint8_t* io_buffer = kmalloc(io_buffer_sec_cnt * SECTOR_SIZE);
    if (io_buffer == NULL) PANIC("sync: fail to malloc io_buffer");

    while (1) {
        for (int c_no = 0; c_no < CHANNEL_NUM; c_no++) {
            for (int d_no = 0; d_no < DEVICE_NUM_PER_CHANNEL; d_no++) {
                struct disk* dev = &channels[c_no].devices[d_no];
                if (dev->name[0] == '\0'|| dev->i_rdev == 0 || dlist_empty(&dev->dirty_lists[dev->active_dirty_idx])) continue;
                lock_acquire(&dev->lists_lock);
                
                // 切换活跃索引，让 bwrite 开始往另一个队列写数据
                // 现在的旧队列进行排序和同步
                int old_idx = dev->active_dirty_idx;
                // 0 变 1，1 变 0
                dev->active_dirty_idx = 1 - old_idx;

                lock_release(&dev->lists_lock);

                if (dlist_empty(&dev->dirty_lists[old_idx])){
                    continue;
                }
                // 在锁外重构顺序，以便于合并 io
                struct dlist sorted_list;
                dlist_init(&sorted_list);
                
                lock_acquire(&global_ide_buffer.lock);    
                while (!dlist_empty(&dev->dirty_lists[old_idx])) {
                    // 将旧队列中的节点弹出，然后按需插入 sorted_list 中
                    // 这个操作的复杂度较高，可能会达到 O(n^2)
                    struct dlist_elem* pelem = dlist_pop_front(&dev->dirty_lists[old_idx]);
                    struct buffer_head* bh = member_to_entry(struct buffer_head, dirty_tag, pelem);
                    // 添加计数，防止被 evict
                    // 不知道为什么，在此处增加 b_ref_count 的计数的话
                    // 基准测试的速度反而还会快 2 秒左右，这可能是因为此处可以合并写回，速度更快一些
                    // 如果不添加计数的话，该块可能会被 evict 出去，evict 在驱逐脏块时，首先会进行一次写回
                    // 然后回到这里后，又会对这个块进行一次写回，不添加计数的话可能会引入 evict 里面那次额外的同步
                    // 因此速度会变慢，变慢都不要紧，两次同步可能还会导致额外的一致性问题，最好此处还是加一下
                    // 这个操作的本质其实是一个缓存锁定操作
                    bh->b_ref_count++; 
                    // 使用 insert_order 逻辑，但在私有链表上运行
                    dlist_insert_order(&sorted_list, _cb_bh_lba_condition, &bh->dirty_tag);
                }
                lock_release(&global_ide_buffer.lock);

                // 使用排序后的队列进行 io，速度比不排序直接 io 时可能会提升一倍左右
                while (!dlist_empty(&sorted_list)) {
                    struct buffer_head* batch[MAX_SYNC_COUNT];
                    int count = 0;
                    uint32_t start_lba;

                    lock_acquire(&global_ide_buffer.lock);
                    if (dlist_empty(&sorted_list)) {
                        lock_release(&global_ide_buffer.lock);
                        break;
                    }

                    // 提取连续脏块
                    struct dlist_elem* pelem = dlist_pop_front(&sorted_list);
                    struct buffer_head* bh = member_to_entry(struct buffer_head, dirty_tag, pelem);
                    batch[count++] = bh;
                    start_lba = bh->b_blocknr;

                    while (count < MAX_SYNC_COUNT && !dlist_empty(&sorted_list)) {
                        
                        struct dlist_elem* next_pelem = sorted_list.head.next;
                        struct buffer_head* next_bh = member_to_entry(struct buffer_head, dirty_tag, next_pelem);

                        if (next_bh->b_blocknr == batch[count-1]->b_blocknr + 1) {
                            dlist_pop_front(&sorted_list);
                            batch[count++] = next_bh;
                        } else {
                            break;
                        }
                    }

                    // 先在锁内标记为非脏（防止丢失 IO 期间产生的新修改）
                    // 如果在 ide_write 期间，有进程又改了这个块，它会重新调用 dirty 把这个块再次挂进 dirty_list。
                    // 这样 sync_thread 在下一轮循环中会再次发现它，保证数据最终一定落盘。
                    for (int i = 0; i < count; i++) {
                        batch[i]->b_dirty = false;
                        // 如果有引用等待，可以在这里处理
                    }
                    lock_release(&global_ide_buffer.lock);

                    // 内存拼接，将零散的缓存块数据拷贝到连续的 io_buffer
                    for (int i = 0; i < count; i++) {
                        memcpy(io_buffer + i * SECTOR_SIZE, batch[i]->b_data, SECTOR_SIZE);
                    }

                    // 批量 IO，一次性写入磁盘
                    ide_write(dev, start_lba, io_buffer, count);
                    lock_acquire(&global_ide_buffer.lock);
                    for (int i = 0; i < count; i++) {
                        // brelse(batch[i]); 
                        batch[i]->b_ref_count--;
                    }
                    lock_release(&global_ide_buffer.lock);
                    // printk("\nsync_thread: write %d sectors to dev: 0x%x LBA:0x%x",count, dev->i_rdev, start_lba);
                }
            }
        }

        // printk("sync_thread: sync disk done!\n");
        // 定期休眠
        // 即使被 thread_unblock 强制唤醒也没事
        // 因为 sys_milsleep 中的 thread_block 后有一个将进程移出睡眠队列的操作
        // 可以保证被唤醒后的进程不在睡眠队列中
        sys_milsleep(5000); 
    }
}

// 针对零拷贝的 bread 设计的零拷贝的 write
void bwrite(struct buffer_head* bh) {
    if (bh == NULL) return;
    
    lock_acquire(&global_ide_buffer.lock);
    if (!bh->b_dirty) {
        bh->b_dirty = true;
        // 这里的 bh->b_dev 已经在 bread 的 getblk 时填好了
        // dlist_insert_order(&bh->b_dev->dirty_list, _cb_bh_lba_condition, &bh->dirty_tag);

        // 不要在在此处进行 dlist_insert_order，这太费时间了，在叠加 swap 的情况下可能会引发严重的竞态问题
        // 我们直接在此处进行 O(1) 级别的 push_back，在 sync 的时候再进行重整排序，之后在进行写入 
        lock_acquire(&bh->b_dev->lists_lock);
        // 获取活跃队列，将数据压到活跃队列中
        int32_t active_idx = bh->b_dev->active_dirty_idx;
        dlist_push_back(&bh->b_dev->dirty_lists[active_idx], &bh->dirty_tag);
        lock_release(&bh->b_dev->lists_lock);
    }
    lock_release(&global_ide_buffer.lock);
}

// 完全异步的 io 操作
// 全量延迟写
void bwrite_multi(struct disk* dev, uint32_t start_lba, void* src_buf, uint32_t sec_cnt) {
    for (uint32_t i = 0; i < sec_cnt; i++) {
        uint32_t current_lba = start_lba + i;
        // 没命中就申请一个，命中了就增加引用
        // getblk 内部会处理缓存的负载
        struct buffer_head* bh = getblk(dev, current_lba);
        
        // 既然是全块覆盖写，不需要 read 磁盘。直接 memcpy
        memcpy(bh->b_data, (uint8_t*)src_buf + (i * SECTOR_SIZE), SECTOR_SIZE);
        
        bh->b_valid = true;  // 数据已经是最新的了
        lock_acquire(&global_ide_buffer.lock);
        if (!bh->b_dirty) {
            bh->b_dirty = true;
            // 加入脏队列，按照 lba 升序插入，以便后续合并
            // dlist_insert_order(&dev->dirty_list, _cb_bh_lba_condition, &bh->dirty_tag);

            
            // 不要在在此处进行 dlist_insert_order，这太费时间了，在叠加 swap 的情况下可能会引发严重的竞态问题
            // 我们直接在此处进行 O(1) 级别的 push_back，在 sync 的时候再进行重整排序，之后在进行写入 
            lock_acquire(&dev->lists_lock);
            int32_t active_idx = dev->active_dirty_idx;
            // 获取活跃队列，将数据压到活跃队列中
            dlist_push_back(&dev->dirty_lists[active_idx], &bh->dirty_tag); 
            lock_release(&dev->lists_lock);
        }
        lock_release(&global_ide_buffer.lock);
        brelse(bh); // 只是减少引用，数据还在缓存里，等 sync 线程处理
    }
}

// 直接唤醒 sync 线程来异步清理
void sys_sync(){
    if(sync_thread->status == TASK_WAITING){
        thread_unblock(sync_thread);
    }
}