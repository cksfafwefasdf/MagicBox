#include "inode.h"
#include "fs_types.h"
#include "hashtable.h"
#include "sync.h"
#include "ide.h"
#include "ide_buffer.h"
#include "interrupt.h"
#include "dlist.h"
#include "fs.h"
#include "debug.h"
#include "sifs_inode.h"


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
                hash_remove_elem(&inode_global_cache.hash_table, &victim->hash_tag);
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
        inode_found->i_open_cnts++;
        // 无论 open_cnts 是多少，只要被访问，就更新它在 LRU 中的位置
        // 保证 LRU 队尾永远是最近最活跃的
        dlist_remove(&inode_found->lru_tag);
        dlist_push_back(&inode_global_cache.lru_list, &inode_found->lru_tag);
        lock_release(&inode_global_cache.lock);
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

	// 未命中，从磁盘读取，由于 sifs_read_inode 会用到 i_dev 和 i_no
    // 因此此处要先填充
    if(part->sb->s_magic == SIFS_FS_MAGIC_NUMBER){
        sifs_read_inode(part, new_inode);
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
	if(--inode->i_open_cnts==0){

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