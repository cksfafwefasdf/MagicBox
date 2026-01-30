#ifndef __KERNEL_MEMORY_H
#define __KERNEL_MEMORY_H
#include "../lib/stdint.h"
#include "../lib/kernel/bitmap.h"
#include "../lib/kernel/dlist.h"

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
#define K_HEAP_START 0xc0100000
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

extern struct pool kernel_pool,user_pool; // phisical mem pool
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
extern void* get_a_page_without_op_vaddrbitmap(enum pool_flags pf,uint32_t vaddr);
extern void free_a_phy_page(uint32_t pg_phy_addr);
extern void sys_free_mem(void);
extern void copy_page_tables(struct task_struct* from,struct task_struct* to,void* page_buf);
extern void set_page_read_only(struct task_struct* pthread);
extern void sys_test(void);
extern void swap_page(uint32_t err_code,void* err_vaddr);
extern void write_protect(uint32_t err_code,void* err_vaddr);

extern void* kmalloc(uint32_t size);
extern void* umalloc(uint32_t size);
extern void* do_alloc(uint32_t size, enum pool_flags PF);
extern void ufree(void* ptr);
extern void kfree(void* ptr);
extern void do_free(void* ptr,enum pool_flags PF);


extern uint32_t mem_bytes_total;
#endif