#ifndef __INCLUDE_MAGICBOX_BUDDY_H
#define __INCLUDE_MAGICBOX_BUDDY_H

#include <stdint.h>
#include <dlist.h>
#include <sync.h>
#include <stdbool.h>

struct mem_block_desc;

// 给定物理地址，获取对应的 struct page
#define ADDR_TO_PAGE(page_base,addr) (&page_base[(uint32_t)(addr) >> 12])
// 给定 struct page，获取其物理地址
// 两个地址指针相减，得到的不是地址差，而是元素位置差
// 因此此处不需要除以 sizeof(struct page)
// 如果是 (uint32_t)pg - (uint32_t)global_pages
// 那这就不是地址相减了，而是纯数值相减，得到的就是地址差而不是位置差
// 此时就要除以 sizeof(struct page)
#define PAGE_TO_ADDR(bpool, pg) ((uint32_t)((pg) - (bpool)->page_base) << 12)

// 最大管理 2^10 = 1024 页 (4MB 连续空间)
// 0 ~ 10 分别对应 2^0 到 2^10 页的内存大小
#define MAX_ORDER 11 

struct page {
    // 物理页的标记
    // bit 0: 是否被占用 (Allocated)
    // bit 1: 是否是块的第一个页 (Head)
    // bit 2: 是否属于 Slab
    uint32_t flags;

    uint32_t ref_count; // 引用计数，专门负责 COW 和物理页生命周期

    int32_t order; // 如果是块的头页，记录该块的 order
    // kmalloc arena / large allocation 的元信息，只有头页会使用这些字段。
    // 如果我们 kmalloc 申请的是一个超过 4KB 的块的话，只有第一个页会记录这些属性，其他的不记录
    // 这几个属性就是原本 arena 的属性，我们现在完全把他搬到 page 结构体来了
    struct mem_block_desc* slab_desc; // 指向对应的块描述符
    // when large is true, cnt is the number of page frames
	// for example, when malloc 5000KB, the cnt is 2
	// otherwise it is the number of the free mem_block
    uint32_t slab_cnt;
    bool slab_large; // when malloc above 1024Bytes, large is true
    uint8_t slab_pad[3];
    struct dlist_elem free_list_tag; // 挂载到对应 order 的空闲链表上
    struct dlist_elem activate_tag; // 挂载到对应 order 的空闲链表上
    // 指向 task_struct，用来找到进程的页目录 (pgdir)，记录这个页被哪个进程拥有
    // 由于我们的系统中之前引入了 COW，因此可能会出现多个虚拟地址映射到同一个页上的情况
    // 在这种情况下想进行 swap 是很复杂的，并且其实在我们的系统中，并不会出现大量的共享页情况
    // 大多数情况在 fork 的不久后都会立马触发 COW，使得计数变回 1，因此我们只 swap 那些计数为 1 的页
    struct task_struct* first_owner; 
    uint32_t first_vaddr; // 记录该物理页对应的虚拟地址
};

struct free_area {
    struct dlist free_list; // 该阶所有空闲块的链表
    uint32_t nr_free; // 当前阶空闲块的数量
};

// 物理内存池，替代原来的 struct pool
struct buddy_pool {
    struct free_area areas[MAX_ORDER];
    struct lock lock;
    uint32_t phy_addr_start;
    uint32_t pool_size;
    // 该池对应的 page 数组起始地址，在内核内存管理中，它们都是 global_pages
    struct page* page_base; 
    // 记录可供置换的活跃页，活跃页会在 mapping_v2p 函数中被挂载到这个队列上
    // mapping_v2p 在整个系统中只会被 swap_page 函数调用
    struct dlist activate_list; 
};

extern void pfree_pages(struct buddy_pool* bpool, struct page* pg, uint32_t order);
extern bool page_is_allocated(struct page* pg);
extern struct page* get_buddy_page(struct buddy_pool* bpool, struct page* pg, uint32_t order);
extern struct page* palloc_pages(struct buddy_pool* bpool, uint32_t order);
extern void buddy_init(struct buddy_pool* bpool, uint32_t start_addr, uint32_t size, struct page* page_base);

#endif
