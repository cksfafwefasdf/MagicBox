#ifndef __INCLUDE_MAGICBOX_MEMORY_H
#define __INCLUDE_MAGICBOX_MEMORY_H
#include <stdint.h>
#include <stdbool.h>
#include <bitmap.h>
#include <dlist.h>

#define PG_P_1 1 
#define PG_P_0 0 
#define PG_RW_R 0
#define PG_RW_W 2
#define PG_US_S 0
#define PG_US_U 4
// 16Bytes 32,64,128,256,512,1024 
// 7 types in total
#define DESC_TYPE_CNT 7


// low 1MB is 0xc000000000～0xc00fffff
// heap is close to the low 1MB
// 现在我们的 K_HEAP_START 改用动态计算，其为全局变量 kernel_heap_start
// #define K_HEAP_START 0xc0100000


#define USER_PDE_NR 768
#define USER_PTE_NR 1024
// the size of the item in page table or page directory table
#define TABLE_ITEM_SIZE_BYTES 4

// the number of item in page directory table or page table
#define TABLE_ITEM_NR (PG_SIZE/TABLE_ITEM_SIZE_BYTES)

// 向上对齐到页边界
#define PAGE_ALIGN_UP(vaddr) (((vaddr) + PG_SIZE - 1) & ~(PG_SIZE - 1))
// 向下对齐到页边界（顺便提供，方便以后用）
#define PAGE_ALIGN_DOWN(vaddr) ((vaddr) & ~(PG_SIZE - 1))

#define PDE_IDX(addr) ((addr&0xffc00000)>>22)
#define PTE_IDX(addr) ((addr&0x003ff000)>>12)
// find which start vaddr will map to this pte and pde
#define PTE_TO_VADDR(pde_idx,pte_idx) (((pde_idx)<<22)|((pte_idx)<<12))
#define PAGE_TABLE_VADDR(pde_idx) (0xffc00000|(pde_idx<<12))

#define KERNEL_PAGE_OFFSET 0xC0000000UL
// 需要注意的是，像是PCB，页表这一类及其重要的数据结构
// 都必须放到低端内存中，因为这些内容必须要能持久且快速被内核直接访问到
#define KERNEL_DIRECT_SIZE  0x38000000UL // 896MB
#define KERNEL_DIRECT_END   (KERNEL_PAGE_OFFSET + KERNEL_DIRECT_SIZE)
#define KERNEL_KMAP_START   KERNEL_DIRECT_END // KMAP 地址区域用于映射高端地址， 0xF8000000
// 这里只是124MB，不是128MB，这是因为我们在页目录表的最后一项（1023项）做了一个自映射
// 也就是说最后的那4MB本来就被页表系统占用了，因此这部分地址我们得留出来
// 这4MB的空间我们是用来访问页目录表自身的，所以不能用来做直接映射
// 因为页目录表不一定就在相应地址直接映射的物理页那个地方是
// 并且我们把之前的 K_TEMP_PAGE_VADDR 逻辑给删除了
// 在copy_page_tables函数里接入了kmap逻辑进行高端地址映射从而实现中转

/*
	引入高端地址和低端地址后容易出现困惑的几个地方：
	(1)	用户进程天然可以正常使用映射到 highmem 的物理页，
		因为用户态始终是通过“用户虚拟地址 -> 页表 -> 物理页”来访问内存的。
		用户并不直接感知底层物理页属于 lowmem 还是 highmem。
		真正受 highmem 限制的是内核直接根据物理页来访问页内容的场景，
		这时如果该物理页不在 lowmem，就需要借助 kmap()。
	(2) 假如系统调用时，假如系统调用时，用户传给内核的 buf 所对应的物理页位于 highmem，
		此时内核依然可以直接通过这个用户虚拟地址直接访问它，而不需要 kmap。
		这是因为在陷入内核时，cpu 所作的只是改变当前 cpu 的特权级（比如 CS 寄存器的那几个标志位）
		他不会去改变当前 cr3 所指向的页目录表，因此内核依然可以通过用户传入的虚拟地址直接访问到用户所提供的 buf
		不需要借助 kmap，而由于在我们的设计中，在切换进程时，我们只会拷贝页表的低 768 项
		高 256 项的内核区域自始至终没变过，因此所有用户进程在陷入内核态后能看到的内核区域都是一致的
		因此也可以正常访问到相应的内核区域
*/

#define KERNEL_KMAP_END     0xFFC00000UL // 横跨了 124MB
#define KMAP_SLOT_CNT       ((KERNEL_KMAP_END - KERNEL_KMAP_START) / PG_SIZE)

enum pool_flags{
	PF_KERNEL = 1,
	PF_USER = 2
};

struct mem_block{
	// free_elem is just a signal to point out an addr of mem_block
	// we don't mind it's value
	struct dlist_elem free_elem;
};

struct mem_block_desc{
	uint32_t block_size;
	uint32_t block_per_arena; // num of mem_block in each arena
	struct dlist free_list;
};

struct task_struct;

extern struct buddy_pool kernel_pool,user_pool; // phisical mem pool
extern void mem_init(void);
extern uint32_t* pde_ptr(uint32_t vaddr);
extern uint32_t* pte_ptr(uint32_t vaddr);
extern void* malloc_page(enum pool_flags pf,uint32_t pg_cnt);
extern void* get_kernel_pages(uint32_t pg_cnt);
extern void* get_user_pages(uint32_t pg_cnt);
extern void* mapping_v2p(enum pool_flags pf,uint32_t vaddr);
extern uint32_t addr_v2p(uint32_t vaddr);
extern void* kmap(uint32_t paddr);
extern void kunmap(void* vaddr);
extern bool paddr_is_lowmem(uint32_t paddr);
extern bool vaddr_is_directmap(uint32_t vaddr);
extern bool vaddr_is_kmap(uint32_t vaddr);
extern void block_desc_init(struct mem_block_desc* desc_array);
extern void mfree_page(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt);
extern void pfree(uint32_t pg_phy_addr);
extern void sys_free_mem(void);
extern void sys_test(void);
extern void vaddr_remove(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt);
extern void* palloc(struct buddy_pool* m_pool) ;


extern void* kmalloc(uint32_t size);
extern void* umalloc(uint32_t size);
extern void* do_alloc(uint32_t size, enum pool_flags PF);
extern void ufree(void* ptr);
extern void kfree(void* ptr);
extern void do_free(void* ptr,enum pool_flags PF);


extern uint32_t sys_brk(uint32_t new_brk);


extern uint32_t mem_bytes_total;
extern uint32_t kernel_heap_start;
extern struct page* global_pages;
#endif
