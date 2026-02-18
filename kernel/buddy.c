#include "buddy.h"
#include "debug.h"
#include "print.h"

// 系统刚起来时，伙伴系统还没起来，global_pages 需要绕过伙伴系统特殊处理来存储
void buddy_init(struct buddy_pool* bpool, uint32_t start_addr, uint32_t size, struct page* page_base) {
    lock_init(&bpool->lock);
    bpool->phy_addr_start = start_addr;
    bpool->pool_size = size;
    bpool->page_base = page_base;

    for (int i = 0; i < MAX_ORDER; i++) {
        dlist_init(&bpool->areas[i].free_list);
        bpool->areas[i].nr_free = 0;
    }

    // 将内存划分为尽量大的块，塞进伙伴系统
    uint32_t curr_pfn = start_addr >> 12;
    uint32_t end_pfn = (start_addr + size) >> 12;

    // 假如我们的内存是 31MB此时
    // Order 10 的块会有 7 个
    // Order 9 的块会有 1 个
    // Order 8 的块会有 1 个
    while (curr_pfn < end_pfn) {
        // 找到当前 PFN 能支持的最大对齐 Order
        uint32_t order = MAX_ORDER - 1;
        // 检查对齐, curr_pfn 必须能被 2^order 整除
        // 检查边界, curr_pfn + 2^order 不能超过 end_pfn
        while (order > 0) {
            if ((curr_pfn % (1 << order) == 0) && (curr_pfn + (1 << order) <= end_pfn)) {
                break;
            }
            order--;
        }

        // 将这个“最大可能”的块直接挂入伙伴系统
        struct page* pg = &page_base[curr_pfn];
        pg->order = order;
        pg->flags = 0;
        dlist_push_back(&bpool->areas[order].free_list, &pg->free_list_tag);
        bpool->areas[order].nr_free++;

        // 步进到下一个块
        curr_pfn += (1 << order);
    }
    
}

// 在指定的 buddy_pool 中分配 2^order 个连续物理页
// 例如我们现在想要一个 order = 1 的块，但是 order = 1 的空闲链表和 order = 2 的空闲链表都是空的，但 order = 3 的链表不空
// 那么我们拿一个 order = 3 的空闲块来切割，递归的切割，切割到order=1为止
// 00000000 -> 0000 0000 -> 00 00 0000 分成这样的三个块，第一个块 00 返回给用户用，它的伙伴第二个 00 挂到 order 为 1 的空闲链表上
// 另一组更大的块 0000 挂到 order 为 2 的空闲链表上
// 虽然分配出去的是 2 页（Index 0 和 1），但只有 global_pages[0] 的 order 被设为 1
// flags 被设为 Allocated。global_pages[1] 的状态在伙伴系统视角下是跟随者
// 这没问题，只要释放时传的是 Index 0 的指针就行，伙伴系统中地址的二次幂对齐可以保证这种做法的正确新
struct page* palloc_pages(struct buddy_pool* bpool, uint32_t order) {
    ASSERT(order < MAX_ORDER);
    lock_acquire(&bpool->lock);

    uint32_t k = order;
    // 寻找最近的、有空闲块的阶
    while (k < MAX_ORDER && dlist_empty(&bpool->areas[k].free_list)) {
        k++;
    }

    if (k == MAX_ORDER) { // 内存耗尽
        put_str("Buddy system out of memory! Requested order: "); put_int(order); put_str("\n");
        lock_release(&bpool->lock);
        return NULL;
    }

    // 从链表中摘取一个大块
    struct dlist_elem* elem = dlist_pop_front(&bpool->areas[k].free_list);
    
    ASSERT(elem != &bpool->areas[k].free_list.tail && elem != &bpool->areas[k].free_list.head);

    struct page* pg = member_to_entry(struct page, free_list_tag, elem);
    bpool->areas[k].nr_free--;
    
    // put_str("nr_free--: ");put_int(bpool->areas[k].nr_free);put_str("\n");

    // 向上溯源拿到的块如果比需求大，则执行拆分 (Split)
    while (k > order) {
        k--;
        // 算出“伙伴”页：即当前块的中点地址对应的 page
        struct page* buddy = pg + (1 << k); 

        uint32_t buddy_phy = PAGE_TO_ADDR(bpool, buddy);

        // 检查这个 buddy 是否还在当前 pool 管理的 page 范围内
        if (buddy_phy >= bpool->phy_addr_start + bpool->pool_size) {
            // 如果越界了，不能挂进链表
            continue; 
        }
        
        // 初始化伙伴页属性并挂入低一阶的空闲链表
        buddy->order = k;
        buddy->flags = 0; // 标记为空闲
        dlist_push_back(&bpool->areas[k].free_list, &buddy->free_list_tag);
        bpool->areas[k].nr_free++;
    }

    // 标记当前分配出去的块
    pg->order = order;
    pg->flags |= 1; // 假设 bit 0 为 Allocated
    
    lock_release(&bpool->lock);
    return pg;
}

// 主要为 pfree 所用
// 假设 0000 1111 1111 0000 当中每一位相当于一个页，相邻的两个页物理地址连续，1表示占用，0表示空闲
// 现在我们要释放 pg_idx = 4 = 0b0100 order = 2 的一个块的内存
// 计算得到 buddy_idx = 0，它的伙伴是最左边那个元素起始的4个页
// 若我们要释放 pg_idx = 8 = 0b1000，order = 2 的一个块的内存
// 那么 buddy_idx = 0b1100 = 12
// 也就是说，若该块在“左”（即order位为0），那么它就找右边（order位为1）的2^order个块是否空闲
// 若在右，那么就找左边的 2^order 个块是否空闲

// 在伙伴系统中，并不是任意两个相邻的块都能合并，只有互为“孪生兄弟”的两个块才能合并
// 伙伴系统的合并规则不是看“物理地址是否相邻”，而是看它们是否由同一个大块拆分而来
// 对于 order = 2（4页一组合）的块
// 索引 0, 1, 2, 3 原本是一个order = 3 的块拆出来的，所以 0, 1, 2, 3（统称为块 0）和 4, 5, 6, 7（统称为块 1）是亲兄弟
// 索引 8, 9, 10, 11（块 2）和 12, 13, 14, 15（块 3）是亲兄弟
// 虽然“块 1”和“块 2”在物理地址上也是相邻的，但它们在逻辑上是“邻居”而不是“兄弟”
// 如果允许块 1 (0b01xx) 和块 2 (0b10xx) 合并，那么会得到一个从索引 4 到索引 11 的 8 页区域
// 问题出现了，这个新块的起始地址是 4，它不符合 order = 3（8页一组）的对齐要求（起始地址必须能被 8 整除）
// 如果我们允许这种“非对齐合并”，伙伴系统的高效性会崩塌
// 地址计算变复杂，不能再用简单的 ^ (1 << order) 来找伙伴
// 管理成本激增，我们必须记录每个块的起始位置和长度，而不能只靠一个 order
// 并且碎片化会更严重，这种不规则的合并会导致内存空洞分布得毫无规律，最终无法重新组合成更大的 2^n 块。并且内存中会出现大量长度为 3, 5, 7 页的怪异碎片。
// 很多硬件（如磁盘、网卡）在进行 DMA 传输时，要求内存地址必须是对齐的（比如 16KB 传输要求地址能被 16KB 整除）。伙伴系统天生就满足了这个需求
// 换句话说，我们可以让 0b00 00 ~ 0b00 11 以及 0b01 00 ~ 0b01 11 这 0b0 000 ~ 0b 0 111 页组成一个编号为 0b1，order 为 3 的块
// 伙伴系统中如果它的兄弟不是空闲的，但是它的另一个不是兄弟的邻居是空闲的，那么系统就宁愿不进行合并
struct page* get_buddy_page(struct buddy_pool* bpool, struct page* pg, uint32_t order) {
    // 这里的偏移量是相对于 global_pages 数组基址的索引
    uint32_t pg_idx = pg - bpool->page_base;
    // 异或操作，翻转第 order 位
    uint32_t buddy_idx = pg_idx ^ (1 << order);

    // 检查 buddy_idx 是否在当前 pool 的物理页范围内
    uint32_t pool_start_pfn = bpool->phy_addr_start >> 12;
    uint32_t pool_end_pfn = (bpool->phy_addr_start + bpool->pool_size) >> 12;

    if (buddy_idx < pool_start_pfn || buddy_idx >= pool_end_pfn) {
        return NULL; // 这是一个孤儿块，没有伙伴
    }

    return &bpool->page_base[buddy_idx];
}

// 检查 page 是否被分配
bool page_is_allocated(struct page* pg) {
    return (pg->flags & 1);
}

void pfree_pages(struct buddy_pool* bpool, struct page* pg, uint32_t order) {
    ASSERT(order < MAX_ORDER);
    lock_acquire(&bpool->lock);

    uint32_t k = order;
    struct page* curr = pg;

    // 尝试递归合并
    while (k < MAX_ORDER - 1) {
        // 找到当前块在当前阶(k)下的伙伴
        struct page* buddy = get_buddy_page(bpool,curr, k);

        uint32_t buddy_phy_addr = PAGE_TO_ADDR(bpool,buddy); 

        // 伙伴的物理地址是否超出了当前 pool 的边界
        // 防止出现内核内存池的一个块和用户内存池的一个块是逻辑上的伙伴
        // 让后把它们两个合并到一起的情况
        if (buddy_phy_addr < bpool->phy_addr_start || 
            buddy_phy_addr >= bpool->phy_addr_start + bpool->pool_size) {
            break; 
        }

        // 检查伙伴是否可以合并
        // 伙伴必须也在这个 pool 的物理范围内
        // 伙伴必须是空闲的 (flags bit 0 == 0)
        // 伙伴当前的阶数必须和自己一致 (防止合并了正在被拆分的块)
        if (page_is_allocated(buddy) || buddy->order != (int32_t)k) {
            // 伙伴不空闲或阶数不对，停止合并
            break;
        }

        // 既然要合并，把伙伴从它所在的阶(k)的空闲链表中摘除
        dlist_remove(&buddy->free_list_tag);
        bpool->areas[k].nr_free--;

        // 找到合并后的新块起始地址
        // 如果当前块地址比伙伴大，则新块起始地址是伙伴的地址
        if (curr > buddy) {
            curr = buddy;
        }

        // 步入更高阶
        k++;
        curr->order = k; 
    }

    ASSERT(!dlist_find(&bpool->areas[k].free_list, &curr->free_list_tag));

    // 将最终合并完成的块挂入对应阶的空闲链表
    curr->flags &= ~1; // 清除已分配标记 (bit 0 = 0)
    curr->order = k; // 设置最终的 order
    dlist_push_front(&bpool->areas[k].free_list, &curr->free_list_tag);
    bpool->areas[k].nr_free++;

    lock_release(&bpool->lock);
}