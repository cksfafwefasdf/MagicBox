#include <inode.h>
#include <fs_types.h>
#include <hashtable.h>
#include <sync.h>
#include <ide.h>
#include <ide_buffer.h>
#include <interrupt.h>
#include <dlist.h>
#include <fs.h>
#include <debug.h>
#include <sifs_inode.h>
#include <ext2_inode.h>

#define MAX_INODE_CACHE_SIZE 64
#define BUCKET_NR 32

/*
    该文件中的是 VFS 通用的inode操作，与文件系统无关
*/

// 用于哈希表查找的 Key
struct inode_key {
    uint32_t i_dev; // inode 所存储在的设备的逻辑 id
    uint32_t i_no;
};

// 每一个inode都要同时位于哈希表和lru队列中
// lru 的作用是用于快速淘汰过时节点
// 当触发 lru 队列满员淘汰时，要将inode同时移出lru和哈希表
struct inode_cache {
    struct lock lock;
    struct hashtable hash_table;
    struct dlist lru_list;
};

struct inode_cache inode_global_cache;

// 哈希计算，使用和ide_buffer一样的黄金分割打散算法
static uint32_t inode_hash(void* arg) {
    struct inode_key* key = (struct inode_key*)arg;
    uint32_t val = key->i_dev ^ key->i_no;
    return val * HASH_GOLDEN_RATIO_32;
}

// 匹配条件，用于处理同一个 bucket 中的冲突项
static bool inode_condition(struct dlist_elem* pelem, void* arg) {
    struct inode_key* ik = (struct inode_key*)arg;
    struct inode* i = member_to_entry(struct inode, hash_tag, pelem);
    return (i->i_dev == ik->i_dev && i->i_no == ik->i_no);
}

void inode_cache_init() {

    lock_init(&inode_global_cache.lock);
    
    hash_init(&inode_global_cache.hash_table, BUCKET_NR, inode_hash, inode_condition);
    dlist_init(&inode_global_cache.lru_list);
}

int32_t inode_register_to_cache(struct inode* inode){
    lock_acquire(&inode_global_cache.lock);

    struct inode_key ik = {inode->i_dev, inode->i_no};

    // 真正插入
    hash_insert(&inode_global_cache.hash_table, &ik, &inode->hash_tag);
    dlist_push_back(&inode_global_cache.lru_list, &inode->lru_tag);

    // 检查是否溢出，若移除则要从lru中淘汰一个inode
    if (inode_global_cache.hash_table.elem_nr > MAX_INODE_CACHE_SIZE) {
        // 从 LRU 队首开始找，直到找到一个可以被淘汰的（i_open_cnts == 0）
        struct dlist_elem* pelem = inode_global_cache.lru_list.head.next;
        while (pelem != &inode_global_cache.lru_list.tail) {
            struct inode* victim = member_to_entry(struct inode, lru_tag, pelem);
            
            if (victim->i_open_cnts == 0) {
                // 真正从系统中抹除这个 inode
                hash_remove(&inode_global_cache.hash_table, &victim->hash_tag);
                dlist_remove(&victim->lru_tag);
                kfree(victim); 
                break; // 腾出一个位置就行
            }
            pelem = pelem->next;
        }
    }
    lock_release(&inode_global_cache.lock);
    return 0;
}

// 将 inode 的缓存强制从内存中驱逐
void inode_evict(struct inode* inode) {
    if (inode == NULL) return;

    lock_acquire(&inode_global_cache.lock);
#ifdef DEBUG_INODE_CACHE
    PUTS("evict inode: ",inode);
    PUTS("evict ino: ",inode->i_no);
#endif
   
    // 从全局哈希表中移除
    // 这样之后任何 inode_open 都会因为找不到而触发重新读盘（读到已删除状态）
    // 最好用 hash_remove_elem，它是基于dlist_remove直接实现的
    // 不依赖于 condition 函数，这样的话即使key损坏了（比如实现释放了这块区域的内存，之后再移除缓存）也不会出错
    // 能用物理地址解决的决不要用逻辑标识解决，因为物理地址是唯一的！
    hash_remove(&inode_global_cache.hash_table, &inode->hash_tag);
    // 从 LRU 链表中移除
    dlist_remove(&inode->lru_tag);
    lock_release(&inode_global_cache.lock);

    // 释放关联资源，主要针对fifo和pipe
    // 只要指向的内存不为空就强制清除其下的内存
    if (inode->i_type == FT_FIFO || inode->i_type == FT_PIPE) {
        if (inode->pipe_i.base != 0) {
            mfree_page(PF_KERNEL, (void*)inode->pipe_i.base, 1);
            inode->pipe_i.base = 0;
        }
    }
    // memset(inode,0,sizeof(struct inode));
    // 彻底销毁内存对象
    inode->hash_tag.prev = inode->hash_tag.next = NULL;
    inode->lru_tag.prev = inode->lru_tag.next = NULL;
    kfree(inode);
}

// load the inode from disk into memory
struct inode* inode_open(struct partition* part,uint32_t inode_no){

	struct inode_key ik = {part->i_rdev, inode_no};
	struct dlist_elem* de = NULL;
    struct inode* inode_found = NULL;

    lock_acquire(&inode_global_cache.lock);
	// 查找哈希表
    de = hash_find(&inode_global_cache.hash_table, &ik);
    if (de != NULL) {
        inode_found = member_to_entry(struct inode, hash_tag, de);

        // 如果这里崩了，说明你的缓存里可能存进了脏东西！
        // 用于诊断缓存不一致的问题
        ASSERT(inode_found->i_no == inode_no && inode_found->i_dev == part->i_rdev);

        inode_found->i_open_cnts++;
        // 无论 open_cnts 是多少，只要被访问，就更新它在 LRU 中的位置
        // 保证 LRU 队尾永远是最近最活跃的
        dlist_remove(&inode_found->lru_tag);
        dlist_push_back(&inode_global_cache.lru_list, &inode_found->lru_tag);
        lock_release(&inode_global_cache.lock);

#ifdef DEBUG_INODE_CACHE
        PUTS("cache hit ino: ",inode_found->i_no);
        PUTS("cache hit inode: ",inode_found);
#endif

        return inode_found;
    }
    lock_release(&inode_global_cache.lock);

    // 读盘这种耗时操作不要拿锁
    struct inode* new_inode = (struct inode*)kmalloc(sizeof(struct inode));
    if (new_inode == NULL) PANIC("alloc memory failed!");

    new_inode->i_no = inode_no;
	new_inode->i_dev = part->i_rdev;
	new_inode->i_open_cnts = 1;
	new_inode->write_deny = false;
    new_inode->i_sb = part->sb; // 建立归属超级块，以后读写数据块要用到他
    new_inode->i_mount = NULL; // 默认不是挂载点
    new_inode->i_mount_at = NULL;// 默认不是另一个分区的根

	// 未命中，从磁盘读取，由于 read_inode 会用到 i_dev 和 i_no
    // 因此此处要先填充
    if(part->sb->s_op != NULL){
        part->sb->s_op->read_inode(new_inode);
    }else{
        PANIC("Unknown file system!");
    }

    // 插入哈希表
    // Double-Check，防止并发 open 导致重复创建
    // 如果两个进程同时 inode_open 同一个不存在于缓存的文件：
    // A 没搜到，去读盘了。
    // B 也没搜到，去读盘了。
    // A 读完，拿锁，插入哈希表。
    // B 读完，拿锁，再次插入。
    // 此时就会重复创建
    lock_acquire(&inode_global_cache.lock);

    de = hash_find(&inode_global_cache.hash_table, &ik);

    if (de != NULL) {
        kfree(new_inode); // 别人已经建好了，这个就不要了
        inode_found = member_to_entry(struct inode, hash_tag, de);
        inode_found->i_open_cnts++;
        // 更新 LRU 位置
        dlist_remove(&inode_found->lru_tag);
        dlist_push_back(&inode_global_cache.lru_list, &inode_found->lru_tag);
    } else {
        // inode_register_to_cache 里面会拿锁，但是由于锁是可重入的
        // 因此不需要释放
        inode_register_to_cache(new_inode);
        inode_found = new_inode;
    }

    lock_release(&inode_global_cache.lock);
	
	ASSERT((uint32_t)inode_found>=kernel_heap_start);
	
	// printk("inode flag::: %x\n",inode_found->write_deny);
	return inode_found;
}

void inode_close(struct inode* inode){
	if (inode == NULL) return;
    lock_acquire(&inode_global_cache.lock); // 必须拿锁，保护全局缓存一致性
	if(--inode->i_open_cnts<=0){

		// 对 FIFO 和 PIPE 的缓冲区进行回收
		// 让缓冲区随inode的消亡同时消亡，保证强一致性
        // fifo 是有磁盘inode的，是通过sys_open打开的
        // 底层会调用 inode_open，会进入inode缓存
        // 但是我们不用额外管理他，把他当普通文件来让lru淘汰就行
        if (inode->i_type == FT_FIFO || inode->i_type == FT_PIPE) {
            if (inode->pipe_i.base != 0) {
                mfree_page(PF_KERNEL, (void*)inode->pipe_i.base, 1);
                inode->pipe_i.base = 0;
            }
        }
        
        // 只有当 LRU 链表太长，或者系统内存不足需要剔除缓存时，从哈希表删除并 kfree。
        // 这个删除操作是由 inode_open 来做的，不是close来做，所以close的操作主要就是减一下计数器

		// 非匿名管道再进行释放，匿名inode是不会出现在打开队列上的
		// if(inode->i_no!=ANONY_I_NO){
        // 	dlist_push_back(&inode_global_cache.lru_list, &inode->lru_tag);
		// }
	}
    lock_release(&inode_global_cache.lock);
}

// 创建匿名 inode
// 只在内存中创建，不去操作磁盘，主要是匿名 pipe 来用
struct inode* make_anonymous_inode() {
    struct inode* inode = (struct inode*)kmalloc(sizeof(struct inode));
    if (inode == NULL) return NULL;

    memset(inode, 0, sizeof(struct inode));

    // 核心身份标识
    inode->i_no = ANONY_I_NO; // 使用-1标志匿名inode
    inode->i_dev = -1; // 使用 -1 （全1）标志其没有存储设备
    inode->i_type = FT_PIPE;
    
    // 初始化引用计数：由 sys_pipe 进一步管理
    inode->i_open_cnts = 0; 

    // 注意！不要执行 dlist_pusb_back(&open_inodes, &inode->inode_tag) 
    // 匿名管道永远都不需要被搜索

    return inode;
}

// 该函数的作用主要是将swap_page函数从具体的文件系统中解耦
int32_t inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count) {
    if(inode->i_sb->s_magic == SIFS_FS_MAGIC_NUMBER){
        return sifs_inode_read_data(inode,offset,buf,count);
    }else if(inode->i_sb->s_magic == EXT2_MAGIC_NUMBER){
        return ext2_inode_read_data(inode,offset,buf,count);
    }else{
        PANIC("unkonwn fs type!");
    }
    return -1;
}