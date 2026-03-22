#ifndef __INCLUDE_MAGICBOX_MEMORY_H
#define __INCLUDE_MAGICBOX_MEMORY_H
#include <stdint.h>
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


// the base-addr of stack used by kernel is 0xc009f000
// we are trying to organize 512MB phy-mem,which needs 512MB/4KB -> 2^17bit=128Kb=16KB for bitmap
// 16KB/[4KB/page] = 4 pages
// so we need 4 pages to store the bitmap,these page need to place into the low 1MB kernel space
// PCB of kernel use 1page(0x9e000~0x9efff,stack-base is 0x9efff+1=0x9f000)
// pages for phy-mem need place into the addr: 0x9e000-(0x1000*4)=0xc009a000`
// so bitmap-base is 0xc009a000
#define MEM_BITMAP_BASE 0xc009a000

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

#define K_TEMP_PAGE_VADDR 0xff3ff000  

enum pool_flags{
	PF_KERNEL = 1,
	PF_USER = 2
};

// virtual mem-pool, the size is 4GB
struct virtual_addr{
	struct bitmap vaddr_bitmap; // virtual mem pool
	uint32_t vaddr_start;
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
extern struct dlist kernel_vma_list;
extern struct lock kernel_vma_lock;
extern struct page* global_pages;
#endif