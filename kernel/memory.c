#include "memory.h"
#include "print.h"
#include "debug.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "sync.h"
#include "interrupt.h"
#include "stdio-kernel.h"
#include "bitmap.h"



#define PDE_IDX(addr) ((addr&0xffc00000)>>22)
#define PTE_IDX(addr) ((addr&0x003ff000)>>12)

// phy-mem pool
struct pool{
	struct bitmap pool_bitmap; // use to organize the physical_addr of the pool
	struct lock lock; // phy mem pool is a public res
	uint32_t phy_addr_start;
	uint32_t pool_size;
	uint32_t pool_pages; // how many pages the pool has, to cooperate with pool_bitmap
};

struct arena{
	// point to the mem_block which is related to this arena
	struct mem_block_desc* desc;
	// when large is true, cnt is the number of page frames
	// for example, when malloc 5000KB, the cnt is 2
	// otherwise it is the number of the free mem_block
	uint32_t cnt;
	bool large; // when malloc above 1024Bytes, large is true
};

struct mem_block_desc k_block_descs[DESC_TYPE_CNT];

struct pool kernel_pool,user_pool;
struct virtual_addr kernel_vaddr;

uint32_t mem_bytes_total = 0;


static void mem_pool_init(uint32_t all_mem){
	put_str("mem_pool init start\n");
	
	lock_init(&kernel_pool.lock);
	lock_init(&user_pool.lock);

	// [total page table size] = [PDT itself](1pg) + [item-0 and item-768 point same place,low 4MB](1pg)+[item-769](1pg)+...+[item-1022](1pg)
	// = 256 pg
	// page-table use 256 pages by itself
	uint32_t page_table_size = PG_SIZE*256;
	uint32_t used_mem = page_table_size + 0x100000; // low 1MB is kernel
	uint32_t free_mem = all_mem-used_mem;
	uint16_t all_free_pages = free_mem/PG_SIZE;
	uint16_t kernel_free_pages = all_free_pages/2; 
	uint16_t user_free_pages = all_free_pages-kernel_free_pages;


	// length of kernel Bitmap
	uint32_t kbm_length = kernel_free_pages/8;
	// use extra 1 byte to store the remainder
	//if(kernel_free_pages%8!=0) kbm_length++;
	// length of user Bitmap
	uint32_t ubm_length = user_free_pages/8;
	//if(user_free_pages%8!=0) ubm_length++;

	kernel_pool.pool_pages = kernel_free_pages;
	user_pool.pool_pages = user_free_pages;

	// Kernel Pool Start
	uint32_t kp_start = used_mem;
	// User pool start
	uint32_t up_start = kp_start+kernel_free_pages*PG_SIZE;

	kernel_pool.phy_addr_start = kp_start;
	user_pool.phy_addr_start = up_start;

	kernel_pool.pool_size = kernel_free_pages*PG_SIZE;
	user_pool.pool_size = user_free_pages*PG_SIZE;

	kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
	user_pool.pool_bitmap.btmp_bytes_len = ubm_length;

	// bitmap.bits is the start-addr of the bitmap
	kernel_pool.pool_bitmap.bits = (void*) MEM_BITMAP_BASE;
	user_pool.pool_bitmap.bits = (void*) (MEM_BITMAP_BASE+kbm_length);


	put_str("kernel_pool_bitmap addr start with: \n");
	put_str("\t"); put_int((int)kernel_pool.pool_bitmap.bits);put_str("\n");
	put_str("kernel_pool_phy_addr start with: \n");
	put_str("\t"); put_int(kernel_pool.phy_addr_start);put_str("\n");
	put_str("user_pool_bitmap addr start with: \n");
	put_str("\t"); put_int((int)user_pool.pool_bitmap.bits);put_str("\n");
	put_str("user_pool_phy_addr start with: \n");
	put_str("\t"); put_int(user_pool.phy_addr_start);put_str("\n");

	//clear bitmap
	bitmap_init(&kernel_pool.pool_bitmap);
	bitmap_init(&user_pool.pool_bitmap);

	kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;

	// virtual mem pool for kernel
	kernel_vaddr.vaddr_bitmap.bits = (void *)(MEM_BITMAP_BASE + kbm_length + ubm_length);
	
	kernel_vaddr.vaddr_start = K_HEAP_START;
	bitmap_init(&kernel_vaddr.vaddr_bitmap);
	put_str("mem_pool_init done\n");
}

// allocate [pg_cnt] pages from [pool_flags] mem-pool
static void* vaddr_alloc(enum pool_flags pf,uint32_t pg_cnt){
	int vaddr_start = 0,bit_idx_start=-1;
	uint32_t cnt = 0;
	if(pf==PF_KERNEL){
		bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap,pg_cnt);
		if(bit_idx_start==-1) return NULL;
		while(cnt<pg_cnt){
			bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx_start+cnt,1);
			cnt++;
		} 
		vaddr_start = kernel_vaddr.vaddr_start+bit_idx_start*PG_SIZE;
	}else{
		// allocate vaddr for user pool
		struct task_struct* cur = get_running_task_struct();
		bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap,pg_cnt);
		if(bit_idx_start==-1) return NULL;
		while(cnt<pg_cnt){
			bitmap_set(&cur->userprog_vaddr.vaddr_bitmap,bit_idx_start+cnt,1);
			cnt++;
		}
		vaddr_start = cur->userprog_vaddr.vaddr_start+bit_idx_start*PG_SIZE;
		ASSERT((uint32_t)vaddr_start<(0xc0000000-PG_SIZE));
	}
	return (void*)vaddr_start;
}

// get vaddr's pte pointer
uint32_t* pte_ptr(uint32_t vaddr){
	uint32_t* pte = (uint32_t*)(0xffc00000+((vaddr&0xffc00000)>>10)+PTE_IDX(vaddr)*4);
	return pte;
}

//get vaddr's pde pointer
uint32_t* pde_ptr(uint32_t vaddr){
	uint32_t* pde = (uint32_t*)((0xfffff000)+PDE_IDX(vaddr)*4);
	return pde;
}

// allocate one phy-page from the phy_pool that m_pool points to
static void* palloc(struct pool* m_pool){
	int bit_idx = bitmap_scan(&m_pool->pool_bitmap,1);
	if(bit_idx==-1) return NULL;
	bitmap_set(&m_pool->pool_bitmap,bit_idx,1);
	uint32_t page_phyaddr = ((bit_idx*PG_SIZE)+m_pool->phy_addr_start);
	return (void *)page_phyaddr;
}

// add relation between _vaddr and _page_phyaddr
static void page_table_add(void* _vaddr,void* _page_phyaddr){
	uint32_t vaddr = (uint32_t)_vaddr,page_phyaddr = (uint32_t)_page_phyaddr;
	uint32_t* pde = pde_ptr(vaddr);
	uint32_t* pte = pte_ptr(vaddr);
	// the lowest bit is P bit
	// check if the page exists in the mem
	if(*pde&0x00000001){
		ASSERT(!(*pte&0x00000001));
		if(!(*pte&0x00000001)){
			*pte = (page_phyaddr|PG_US_U|PG_RW_W|PG_P_1);
		}else{
			PANIC("Duplicate PTE");
			*pte = (page_phyaddr|PG_US_U|PG_RW_W|PG_P_1);
		}
	}else{
		uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
		*pde = (pde_phyaddr|PG_US_U|PG_RW_W|PG_P_1);
		memset((void*)((int)pte&0xfffff000),0,PG_SIZE);

		ASSERT(!(*pte&0x00000001));
		*pte = (page_phyaddr|PG_US_U|PG_RW_W|PG_P_1);
	}	
}

// allocate [pg_cnt] pages from [pf] phy-mem pool
// if successful, return the virtual addr
void* malloc_page(enum pool_flags pf,uint32_t pg_cnt){

	uint32_t max_page = (pf==PF_KERNEL?kernel_pool.pool_pages:user_pool.pool_pages);
	ASSERT(pg_cnt>0&&pg_cnt<max_page);

	void* vaddr_start = vaddr_alloc(pf,pg_cnt);
	if(vaddr_start==NULL) return NULL;

	uint32_t vaddr = (uint32_t)vaddr_start,cnt = pg_cnt;
	struct pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;

	// virtual-page is consecutive but phy-page is not
	// we can only allocate phy-page one by one
	// so we need create the relation between virtual-page and phy-page one by one
	while (cnt-->0){
		void* page_phyaddr = palloc(mem_pool);
		if(page_phyaddr==NULL) return NULL;
		
		page_table_add((void*)vaddr,page_phyaddr);
		vaddr+=PG_SIZE;
	}
	return vaddr_start;
}

// allocate 1 page from kernel phy-mem pool
// if successful, return the virtual addr
void* get_kernel_pages(uint32_t pg_cnt){
	void* vaddr = malloc_page(PF_KERNEL,pg_cnt);

	if(vaddr!=NULL){
		memset(vaddr,0,pg_cnt*PG_SIZE);
	}

	return vaddr;
}


void mem_init(void){
	put_str("mem_init start\n");
	mem_bytes_total = *((uint32_t*)(SYS_MEM_SIZE_PTR));
	mem_pool_init(mem_bytes_total);
	block_desc_init(k_block_descs);
	put_str("mem_init done\n");
}

// allocate 4KB mem from user phy mem, return the vaddr
void* get_user_pages(uint32_t pg_cnt){
	lock_acquire(&user_pool.lock);
	void* vaddr = malloc_page(PF_USER,pg_cnt);
	memset(vaddr,0,PG_SIZE*pg_cnt);
	lock_release(&user_pool.lock);
	return vaddr;
}

// create relationship between vaddr and paddr in [pf] pool
// we cannot control the value of the paddr !!! it is decided by OS !
// only surport to allocate 1 page
void* mapping_v2p(enum pool_flags pf,uint32_t vaddr){
	struct pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;
	lock_acquire(&mem_pool->lock);
	struct task_struct* cur = get_running_task_struct();
	int32_t bit_idx = -1;

	if(cur->pgdir!=NULL&&pf==PF_USER){
		// if cur is a user proc
		// check which pages do vaddr in
		bit_idx = (vaddr-cur->userprog_vaddr.vaddr_start)/PG_SIZE;
		ASSERT(bit_idx>=0);
		bitmap_set(&cur->userprog_vaddr.vaddr_bitmap,bit_idx,1);

	}else if(cur->pgdir==NULL&&pf==PF_KERNEL){
		// if cur is a kernel thread
		bit_idx = (vaddr-kernel_vaddr.vaddr_start)/PG_SIZE;
		ASSERT(bit_idx>0);
		bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx,1);
	}else{
		PANIC("get_a_page: not allow kernel to alloc userspace or user to alloc kernelspace by get_a_page!!!\n");
	}

	void *page_phyaddr = palloc(mem_pool);
	if(page_phyaddr==NULL){
		lock_release(&mem_pool->lock);
		return NULL;
	}
	page_table_add((void*)vaddr,page_phyaddr);

	lock_release(&mem_pool->lock);

	return (void*)vaddr;
}

// use vaddr to get paddr
uint32_t addr_v2p(uint32_t vaddr){
	// all of the ptrs is vaddr
	// so pte is vaddr
	uint32_t* pte = pte_ptr(vaddr);
	// *pte is paddr
	// vaddr&0x00000fff to get offset in low 12bits
	return ((*pte&0xfffff000)+(vaddr&0x00000fff));
}

void block_desc_init(struct mem_block_desc* desc_array){
	uint16_t desc_idx,block_size = 16;
	for(desc_idx=0;desc_idx<DESC_TYPE_CNT;desc_idx++){
		desc_array[desc_idx].block_size = block_size;
		// sizeof(struct arena) is the meta info, in struct arena
		desc_array[desc_idx].block_per_arena = (PG_SIZE-sizeof(struct arena))/block_size;

		dlist_init(&desc_array[desc_idx].free_list);
		block_size*=2;
	}
}

// return the addr of the [idx]th block in the arena
static struct mem_block* arena2block(struct arena* a,uint32_t idx){
	return (struct mem_block*) \
	((uint32_t)a + sizeof(struct arena) + idx*a->desc->block_size);
}

static struct arena* block2arena(struct mem_block* b){
	return (struct arena*)((uint32_t)b&0xfffff000);
}

// the granularity of size is 1byte 
void* sys_malloc(uint32_t size){
	enum pool_flags PF;
	struct pool* mem_pool;
	uint32_t pool_size;
	struct mem_block_desc* descs;
	struct task_struct* cur_thread = get_running_task_struct();

	// which pool will we use
	if(cur_thread->pgdir == NULL){
		// kernel pool
		PF = PF_KERNEL;
		pool_size = kernel_pool.pool_size;
		mem_pool = &kernel_pool;
		descs = k_block_descs;
	}else{
		PF = PF_USER;
		pool_size = user_pool.pool_size;
		mem_pool = &user_pool;
		descs = cur_thread->u_block_desc;
	}
	// if mem allocated above the pool
	if(!(size>0&&size<pool_size)){
		return NULL;
	}
	

	struct arena* a;
	struct mem_block* b;
	lock_acquire(&mem_pool->lock);
	

	// if size above 1024Bytes, allocate 1 page directly
	if(size>1024){
		uint32_t page_cnt = DIV_ROUND_UP(size+sizeof(struct arena),PG_SIZE);

		a = malloc_page(PF,page_cnt);

		if(a!=NULL){
			memset(a,0,page_cnt*PG_SIZE);
			a->desc = NULL;
			a->cnt = page_cnt;
			a->large = true;
			lock_release(&mem_pool->lock);
			// skip the meta info in arena, return the mem pool
			// because [a] is a pointer whose type is arena
			// so a+1 means a+(sizeof(arena)bytes)
			return (void*)(a+1); 
		}else{
			lock_release(&mem_pool->lock);
			return NULL;
		}

	}else{
		// if malloc size belows 1024Bytes
		// use mem_block_desc to allocate 
		uint8_t desc_idx;
		for(desc_idx=0;desc_idx<DESC_TYPE_CNT;desc_idx++){
			// find suitable desc type
			if(size<=descs[desc_idx].block_size) break;
		}
		
		// if free list is empty, then allocate an arena
		if(dlist_empty(&descs[desc_idx].free_list)){
			
			a = malloc_page(PF,1);
			
			if(a==NULL){
				lock_release(&mem_pool->lock);
				return NULL;
			}
			
			memset(a,0,PG_SIZE);
			

			a->desc = &descs[desc_idx];
			a->large = false;
			a->cnt = descs[desc_idx].block_per_arena;
			uint32_t block_idx;
			enum intr_status old_status = intr_disable();
			
			// append the mem_block to the free_list one by one
			for(block_idx=0;block_idx<descs[desc_idx].block_per_arena;block_idx++){
				b = arena2block(a,block_idx);
				ASSERT(!dlist_find(&a->desc->free_list,&b->free_elem));
				dlist_push_back(&a->desc->free_list,&b->free_elem);
			}
			intr_set_status(old_status);
		}
		
		
		
		// dlist_pop_front returns  dlist_elem *
		// mem_block has member dlist_elem
		struct dlist_elem *tmp = dlist_pop_front(&(descs[desc_idx].free_list));
		b = member_to_entry(struct mem_block,free_elem,tmp);
		memset(b,0,descs[desc_idx].block_size);
		
		a= block2arena(b);
		a->cnt--;
		lock_release(&mem_pool->lock);
		
		
		return (void*)b;
	}
}

// opposite of pmalloc
// free one phy mem page 
void pfree(uint32_t pg_phy_addr){
	struct pool* mem_pool;
	uint32_t bit_idx = 0;
	if(pg_phy_addr>=user_pool.phy_addr_start){
		// if page addr in the user kernel pool
		mem_pool = &user_pool;
		bit_idx = (pg_phy_addr - user_pool.phy_addr_start)/PG_SIZE;
	}else{
		mem_pool = &kernel_pool;
		bit_idx = (pg_phy_addr-kernel_pool.phy_addr_start)/PG_SIZE;
	}
	bitmap_set(&mem_pool->pool_bitmap,bit_idx,0);
}

// set P bit in pte zero 
static void page_table_pte_remove(uint32_t vaddr){
	uint32_t* pte = pte_ptr(vaddr);
	*pte &= ~PG_P_1;
	// refresh TLB
	asm volatile ("invlpg %0"::"m"(vaddr):"memory");
}

// remove [pg_cnt] virtual pages, the beginning of v-page is _vaddr
static void vaddr_remove(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt){
	uint32_t bit_idx_start = 0,vaddr = (uint32_t)_vaddr,cnt=0;
	if(pf==PF_KERNEL){
		bit_idx_start = (vaddr-kernel_vaddr.vaddr_start)/PG_SIZE;
		while(cnt<pg_cnt){
			bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx_start+cnt++,0);
		}
	}else{
		struct task_struct* cur_thread = get_running_task_struct();
		bit_idx_start = (vaddr-cur_thread->userprog_vaddr.vaddr_start)/PG_SIZE;
		while(cnt<pg_cnt){
			bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap,bit_idx_start+cnt++,0);
		}
	}
}

// recycle [pg_cnt] phy pages whose virtual addr is begin with _vaddr
void mfree_page(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt){
	uint32_t pg_phy_addr;
	uint32_t vaddr = (int32_t)_vaddr,page_cnt = 0;
	ASSERT(pg_cnt>=1&&vaddr%PG_SIZE==0);
	pg_phy_addr = addr_v2p(vaddr);
	// 0x102000 = (low 1MB kernel space) + (kernel pdt 1KB) = (kernel pt 1KB) 
	ASSERT((pg_phy_addr%PG_SIZE)==0&&pg_phy_addr>=0x102000);

	if(pg_phy_addr>=user_pool.phy_addr_start){
		vaddr -= PG_SIZE;
		// we can only allocate or release the phy pages one by one
		while(page_cnt<pg_cnt){
			vaddr += PG_SIZE;
			pg_phy_addr = addr_v2p(vaddr);

			ASSERT((pg_phy_addr%PG_SIZE)==0&&pg_phy_addr>=user_pool.phy_addr_start);
			pfree(pg_phy_addr);

			page_table_pte_remove(vaddr);

			page_cnt++;
		}
		// we can allocate or release the virtual pages continuously
		vaddr_remove(pf,_vaddr,pg_cnt);


	}else{
		vaddr -=PG_SIZE;
		while(page_cnt<pg_cnt){
			vaddr+=PG_SIZE;
			pg_phy_addr = addr_v2p(vaddr);
			ASSERT((pg_phy_addr%PG_SIZE)==0&&pg_phy_addr>=kernel_pool.phy_addr_start&&pg_phy_addr<user_pool.phy_addr_start);
			
			pfree(pg_phy_addr);

			page_table_pte_remove(vaddr);

			page_cnt++;
		}

		vaddr_remove(pf,_vaddr,pg_cnt);
	}

}

void sys_free(void* ptr){
	ASSERT(ptr!=NULL);
	if(ptr!=NULL){
		enum pool_flags PF;
		struct pool* mem_pool;

		if(get_running_task_struct()->pgdir==NULL){
			ASSERT((uint32_t)ptr>=K_HEAP_START);
			PF = PF_KERNEL;
			mem_pool = &kernel_pool;
		}else{
			PF = PF_USER;
			mem_pool = &user_pool;
		}

		lock_acquire(&mem_pool->lock);
		struct mem_block* b = ptr;
		struct arena* a = block2arena(b);
		if(!(a->large==0||a->large==1)){
			printk("a large is: %d, addr is: %x\n",a->large,ptr);
		}
		
		ASSERT(a->large==0||a->large==1);
		if(a->desc==NULL&&a->large==true){
			mfree_page(PF,a,a->cnt);
		}else{
			dlist_push_back(&a->desc->free_list,&b->free_elem);
			// check if all mem_block in the arena is empty
			// if so, then release the whole arena 
			if(++(a->cnt)==a->desc->block_per_arena){
				uint32_t block_idx;
				// remove free_elem in the mem_block from the free_list one by one 
				for(block_idx=0;block_idx<a->desc->block_per_arena;block_idx++){
					struct mem_block* b = arena2block(a,block_idx);
					ASSERT(dlist_find(&a->desc->free_list,&b->free_elem));
					dlist_remove(&b->free_elem);
				}
				mfree_page(PF,a,1);
			}
		}
		lock_release(&mem_pool->lock);
	}
}

void* get_a_page_without_op_vaddrbitmap(enum pool_flags pf,uint32_t vaddr){
	struct pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;
	lock_acquire(&mem_pool->lock);
	void *page_phyaddr = palloc(mem_pool);
	if(page_phyaddr==NULL){
		lock_release(&mem_pool->lock);
		return NULL;
	}
	page_table_add((void*)vaddr,page_phyaddr);
	lock_release(&mem_pool->lock);
	return (void*)vaddr;
}

void free_a_phy_page(uint32_t pg_phy_addr){
	struct pool* mem_pool;
	uint32_t bit_idx = 0;
	if(pg_phy_addr>=user_pool.phy_addr_start){
		mem_pool = &user_pool;
		bit_idx = (pg_phy_addr - user_pool.phy_addr_start)/PG_SIZE;
	}else{
		mem_pool = &kernel_pool;
		bit_idx = (pg_phy_addr-kernel_pool.phy_addr_start)/PG_SIZE;
	}
	bitmap_set(&mem_pool->pool_bitmap,bit_idx,0);
}

// this funciotn is called by dlist_traversal
bool sum_free_list(struct dlist_elem *elem, int arg){
	uint32_t* free_elem_num = (uint32_t*) arg;
	*free_elem_num += 1;
	// to ensure traverse the whole list
	return false;
}

uint32_t sys_free_mem(){
	uint32_t free_pages = bitmap_count(&user_pool.pool_bitmap)+bitmap_count(&kernel_pool.pool_bitmap);
	int i;
	uint32_t fragment_space = 0;
	for(i=0;i<DESC_TYPE_CNT;i++){
		if(dlist_empty(&k_block_descs[i].free_list)) continue;
		uint32_t free_elem_num = 0;
		dlist_traversal(&k_block_descs[i].free_list,sum_free_list,&free_elem_num);
		fragment_space += free_elem_num*k_block_descs[i].block_size;
	}
	return free_pages*PG_SIZE + fragment_space;
}