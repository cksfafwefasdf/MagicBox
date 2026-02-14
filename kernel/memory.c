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
#include "thread.h"
#include "process.h"
#include "stdint.h"
#include "vma.h"


#define PDE_IDX(addr) ((addr&0xffc00000)>>22)
#define PTE_IDX(addr) ((addr&0x003ff000)>>12)
// find which start vaddr will map to this pte and pde
#define PTE_TO_VADDR(pde_idx,pte_idx) (((pde_idx)<<22)|((pte_idx)<<12))
#define PAGE_TABLE_VADDR(pde_idx) (0xffc00000|(pde_idx<<12))

#define K_TEMP_PAGE_VADDR 0xff3ff000  

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

uint8_t* mem_map = NULL;

uint32_t mem_bytes_total = 0;
uint32_t total_pages = 0;

static void* palloc(struct pool* m_pool);

int32_t inode_read_data(struct m_inode* inode, uint32_t offset, void* buf, uint32_t count);


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
	enum intr_status old = intr_disable();
	int vaddr_start = 0,bit_idx_start=-1;
	uint32_t cnt = 0;
	if(pf==PF_KERNEL){
		bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap,pg_cnt);
		if(bit_idx_start==-1){
			intr_set_status(old);
			return NULL;
		} 
		while(cnt<pg_cnt){
			bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx_start+cnt,1);
			cnt++;
		} 
		vaddr_start = kernel_vaddr.vaddr_start+bit_idx_start*PG_SIZE;
	}else{
		// allocate vaddr for user pool
		struct task_struct* cur = get_running_task_struct();
		bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap,pg_cnt);
		if(bit_idx_start==-1){
			intr_set_status(old);
			return NULL;
		}
		while(cnt<pg_cnt){
			bitmap_set(&cur->userprog_vaddr.vaddr_bitmap,bit_idx_start+cnt,1);
			cnt++;
		}
		vaddr_start = cur->userprog_vaddr.vaddr_start+bit_idx_start*PG_SIZE;
		ASSERT((uint32_t)vaddr_start<(0xc0000000-PG_SIZE));
	}
	intr_set_status(old);
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
// return the phy_addr
static void* palloc(struct pool* m_pool){
	enum intr_status old = intr_disable();
	int bit_idx = bitmap_scan(&m_pool->pool_bitmap,1);
	if(bit_idx==-1){
		intr_set_status(old);
		return NULL;
	} 
	bitmap_set(&m_pool->pool_bitmap,bit_idx,1);
	uint32_t page_phyaddr = ((bit_idx*PG_SIZE)+m_pool->phy_addr_start);
	mem_map[page_phyaddr >> 12]=1;
	intr_set_status(old);
	return (void *)page_phyaddr;
}

// add relation between _vaddr and _page_phyaddr
static void page_table_add(void* _vaddr,void* _page_phyaddr){
	enum intr_status old = intr_disable();
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

	// 强制刷新 TLB：重新加载 CR3
    // uint32_t pdt_paddr;
    // asm volatile ("mov %%cr3, %0" : "=r" (pdt_paddr));
    // asm volatile ("mov %0, %%cr3" : : "r" (pdt_paddr) : "memory");
	asm volatile ("invlpg %0" : : "m" (*(char*)_vaddr) : "memory");
	intr_set_status(old);
}

// allocate [pg_cnt] pages from [pf] phy-mem pool
// if successful, return the virtual addr
void* malloc_page(enum pool_flags pf,uint32_t pg_cnt){

	uint32_t max_page = (pf==PF_KERNEL?kernel_pool.pool_pages:user_pool.pool_pages);
	ASSERT(pg_cnt>0&&pg_cnt<max_page);

	void* vaddr_start = vaddr_alloc(pf,pg_cnt);
	if(vaddr_start==NULL){
		return NULL;
	}

	uint32_t vaddr = (uint32_t)vaddr_start,cnt = pg_cnt;
	struct pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;

	// virtual-page is consecutive but phy-page is not
	// we can only allocate phy-page one by one
	// so we need create the relation between virtual-page and phy-page one by one
	while (cnt-->0){
		void* page_phyaddr = palloc(mem_pool);
		if(page_phyaddr==NULL){
			return NULL;
		}
		
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

	total_pages = mem_bytes_total / PG_SIZE;
	// 每一个 page 占用一个字节，也就是8位
    mem_map = (uint8_t*)kmalloc(total_pages); 
    
    if (mem_map == NULL) {
        PANIC("Could not allocate mem_map!");
    }

	// 初始化：所有物理页初始计数为 0
    memset(mem_map, 0, total_pages);
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

// 专门给内核模块使用 (如文件系统、驱动)
void* kmalloc(uint32_t size) {
    return do_alloc(size, PF_KERNEL);
}

// 专门给用户进程请求内存使用
void* umalloc(uint32_t size) {
    void* vaddr = do_alloc(size, PF_USER);
    return vaddr;
}


// the granularity of size is 1byte 
void* do_alloc(uint32_t size, enum pool_flags PF){
	struct pool* mem_pool;
	struct mem_block_desc* descs;
	struct task_struct* cur_thread = get_running_task_struct();

	// which pool will we use
	if(PF == PF_KERNEL){
		// kernel pool
		mem_pool = &kernel_pool;
		descs = k_block_descs;
	}else{
		mem_pool = &user_pool;
		descs = cur_thread->u_block_desc;
	}
	// if mem allocated above the pool
	if(!(size>0&&size<mem_pool->pool_size)){
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

	if(mem_map[pg_phy_addr >> 12]<=0){
		printk("mem_map[pg_phy_addr >> 12]: %d pg_phy_addr:%x\n",mem_map[pg_phy_addr >> 12],pg_phy_addr);
		PANIC("bad mem_map[] !");
	}

	mem_map[pg_phy_addr >> 12]--;
	if(mem_map[pg_phy_addr >> 12]!=0){
		return;
	}
	enum intr_status old = intr_disable();

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
	intr_set_status(old);
}

// set P bit in pte zero 
static void page_table_pte_remove(uint32_t vaddr){
	enum intr_status old = intr_disable();
	uint32_t* pte = pte_ptr(vaddr);
	// *pte &= ~PG_P_1;
	*pte = 0;
	// refresh TLB
	asm volatile ("invlpg %0"::"m"(vaddr):"memory");
	intr_set_status(old);
}

// remove [pg_cnt] virtual pages, the beginning of v-page is _vaddr
static void vaddr_remove(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt){
	enum intr_status old = intr_disable();
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
	intr_set_status(old);
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
			// printk("pg_phy_addr:%x\n",pg_phy_addr);
			ASSERT((pg_phy_addr%PG_SIZE)==0&&pg_phy_addr>=kernel_pool.phy_addr_start&&pg_phy_addr<user_pool.phy_addr_start);
			
			pfree(pg_phy_addr);

			page_table_pte_remove(vaddr);

			page_cnt++;
		}

		vaddr_remove(pf,_vaddr,pg_cnt);
	}
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    do_free(ptr, PF_KERNEL);
}

void ufree(void* ptr) {
    if (ptr == NULL) return;
    do_free(ptr, PF_USER);
}

// 核心的内存释放引擎
void do_free(void* ptr,enum pool_flags PF){
	ASSERT(ptr!=NULL);
	if(ptr!=NULL){

		struct pool* mem_pool = (PF == PF_KERNEL) ? &kernel_pool : &user_pool;

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

// this funciotn is called by dlist_traversal
static bool sum_free_list(struct dlist_elem *elem UNUSED, void* arg){
	uint32_t* free_elem_num = (uint32_t*) arg;
	*free_elem_num += 1;
	// to ensure traverse the whole list
	return false;
}

void sys_free_mem(){
	// KF: kernel fragment memory
	// KP: kernel free page memory
	// UP: user free page memory
	uint32_t up = bitmap_count(&kernel_pool.pool_bitmap)*PG_SIZE;
	uint32_t kp = bitmap_count(&user_pool.pool_bitmap)*PG_SIZE;
	int i;
	uint32_t kf = 0;
	for(i=0;i<DESC_TYPE_CNT;i++){
		if(dlist_empty(&k_block_descs[i].free_list)) continue;
		uint32_t free_elem_num = 0;
		dlist_traversal(&k_block_descs[i].free_list,sum_free_list,(void*)&free_elem_num);
		kf += free_elem_num*k_block_descs[i].block_size;
	}
	uint32_t user_total = mem_bytes_total/2;
	uint32_t kernel_total = mem_bytes_total/2;
	printk("total\tfree\tused\t\n%d\t%d\t%d\n",mem_bytes_total,kf+kp+up, mem_bytes_total-(kf+kp+up));
	printk("total[K]: %d\ttotal[U]: %d\nfree[KF+KP]: %d+%d\tfree[UP]: %d\nused[K]: %d\tused[U]: %d\n",kernel_total,user_total,kf,kp,up,kernel_total-kf-kp,user_total-up);
	// printk("free[KF+KP]: %d+%d\tfree[UP]: %d\nused[K]: %d\tused[U]: %d\n",kf,kp,up,kernel_total-kf-kp,user_total-up);
	// printk("total[K+U]\tfree[(KF+KP)+UP]\tused[K+U]\t\n%d+%d\t(%d+%d)+%d\t%d+%d\n",kernel_total,user_total,kf, kp,up,kernel_total-kf-kp,user_total-up);
}

// 将父进程的页表项设置为只读后拷贝给子进程
// 使得子进程和父进程在一开始共享完全一致的物理空间
// 发生写页错误后再开始分家
void copy_page_tables(struct task_struct* from, struct task_struct* to, void* page_buf) {
    ASSERT(page_buf != NULL);
	enum intr_status old_status = intr_disable();
    uint32_t* to_pte_buf = (uint32_t*)page_buf;
    uint32_t* temp_pte_ptr = pte_ptr(K_TEMP_PAGE_VADDR); // 获取中转页的PTE地址

    // 遍历用户空间 PDE
	uint32_t pde_idx = 0;
    for (pde_idx = 0; pde_idx < USER_PDE_NR; pde_idx++) {
        uint32_t* from_pde = from->pgdir + pde_idx;
        if (!(*from_pde & PG_P_1)) continue;

        // 为子进程分配页表物理页
        uint32_t to_pt_pa = (uint32_t)palloc(&kernel_pool);
        if (!to_pt_pa) PANIC("copy_page_tables: palloc failed");

		// copy_page_tables 函数是父进程调用的，pte_ptr取到的是父进程页表项相应的地址
        // 将该物理页映射到父进程的中转虚拟地址，以便从父进程直接写入
		// 这时，如果父进程对 K_TEMP_PAGE_VADDR 进行写操作，其实是对 to_pt_pa 这个物理地址进行写操作
		// 也就是说让父子进程的两个不同的虚拟地址映射到了同一个物理地址
        *temp_pte_ptr = to_pt_pa | PG_P_1 | PG_RW_W;
        asm volatile ("invlpg %0" : : "m" (*(char*)K_TEMP_PAGE_VADDR) : "memory");

        // 获取父进程当前页表的虚拟地址 (利用递归分页)
        uint32_t* from_pte_ptr = (uint32_t*)(0xffc00000 | (pde_idx << 12));
        
        // 准备子进程的页表内容
        memset(to_pte_buf, 0, PG_SIZE);
		uint32_t pte_idx = 0;
        for (pte_idx = 0; pte_idx < USER_PTE_NR; pte_idx++) {
            if (from_pte_ptr[pte_idx] & PG_P_1) {
                uint32_t pa = from_pte_ptr[pte_idx] & 0xfffff000;
                
                // 增加引用计数
                mem_map[pa >> 12]++;

                // 将父进程该页设为只读
                if (from_pte_ptr[pte_idx] & PG_RW_W) {
                    from_pte_ptr[pte_idx] &= ~PG_RW_W;
                }

                // 拷贝 PTE 给子进程 ，此时也是只读
                to_pte_buf[pte_idx] = from_pte_ptr[pte_idx];
            }
        }

        // 直接写入映射好的中转页，即写到了子进程的新页表物理页中
        memcpy((void*)K_TEMP_PAGE_VADDR, to_pte_buf, PG_SIZE);

        // 将该页表挂载到子进程的页目录中
		// 相当于子进程直接将to->pgdir[pde_idx]这个虚拟地址映射到了父进程刚刚处理好的to_pt_pa物理地址上
		// 此时父子进程的两个不同的虚拟地址会指向同一个物理地址，但是没关系，因为下一轮循环中
		// 父进程会重新将 K_TEMP_PAGE_VADDR 映射到新的物理地址上
        to->pgdir[pde_idx] = to_pt_pa | PG_US_U | PG_RW_W | PG_P_1;
    }

    // 清理中转页映射，刷新父进程 TLB
    *temp_pte_ptr = 0;
    // 因为修改了大量父进程的 PTE 属性（RW -> RO），必须重载 CR3 彻底刷新
    page_dir_activate(from);
	intr_set_status(old_status);
}

static bool is_vaddr_mapped(struct task_struct* cur, uint32_t vaddr) {
    // 确保是用户空间地址
    if (vaddr < cur->userprog_vaddr.vaddr_start || vaddr >= 0xc0000000) {
        return false;
    }
    // 计算在位图中的索引
    uint32_t bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
    // 检查位图 bitmap_bit_check
    return bitmap_bit_check(&cur->userprog_vaddr.vaddr_bitmap, bit_idx);
}

// 该函数对应两者情况
// 懒加载/交换：内核分配物理页。
// 非法访问：访问了完全没有映射、或者不属于用户空间（如访问内核空间地址）的内存
void swap_page(uint32_t err_code,void* err_vaddr){
	enum intr_status _old =  intr_disable();
	debug_printk("swap_page:::err_code: %d, err_vaddr: %x\n", err_code, err_vaddr);
	
	struct task_struct* cur = get_running_task_struct();
	uint32_t vaddr = (uint32_t)err_vaddr;

	// 硬件给出的 vaddr 可能是 0xbffffabc，我们需要把它对齐到 0xbffff000
    uint32_t page_vaddr = vaddr & 0xfffff000;

	// 先尝试在 VMA 合同库里找找看，这个地址合法吗？
    struct vm_area* vma = find_vma(cur, page_vaddr);

	if (vma == NULL) {
		printk("VMA Search Failed! vaddr: %x\n", page_vaddr);
		// 打印当前进程所有的 vma 范围，看看 0xBFFFF000 在不在里面
		goto segmentation_fault;
	}
	
	// 如果 vma 都不存在，那么说明不是懒加载，而是真正的段错误 
	if (vma == NULL) {
        goto segmentation_fault;
    }

	// 尝试栈自动扩容，由于我们在process.c中的start_process函数中
	// 只默认为用户进程栈的高4KB映射了物理内存
	// 因此一旦用户栈空间超过 4KB 的话就会出现页错误
    // 我们允许栈最大 8MB，即 [0xBF800000, 0xC0000000)
    // 并且地址必须在用户空间

	// 在我们引入 COW 前，我们每一次fork子进程时，子进程都会完整的把父进程的所有数据结构，包括他的页表和虚拟地址位图都拷贝过来
	// 拷贝完毕后，开始扫描整个虚拟地址位图，若位图为1，则调用 get_a_page_without_op_vaddrbitmap 
	// 在不改动位图的情况下建立相应虚拟地址和一个新的物理地址之间的映射
	// 因此即使父进程需要10KB，子进程只需要3KB，我们最终的子进程都会占用10KB
	// 使用COW后，子进程不额外拷贝父进程的位图了，而是重新自立门户
	// 因此每次子进程初次使用栈时，都会触发栈扩容逻辑
    
	// 由于我们引入了 vma，因此我们现在可以直接通过vma来判断地址是否属于栈空间了
	// 因此原本的判断可以直接删除了，地址是否合法可以方向的完全交给 find_vma 来判断

	// 检查是否已经映射过（物理页是否已存在）
    // 如果 is_vaddr_mapped 为 true 却依然触发缺页，可能是 P=0 但位图没清，
    // 在没有置换到交换分区（Swap Partition）前，这种情况通常是异常。
	if (is_vaddr_mapped(cur, page_vaddr)) {
        // 如果物理页已经存在，但仍然缺页，可能是权限问题（比如写保护）
        // 但写保护通常由 write_protect 处理，这里直接 panic 方便调试
        PANIC("swap_page: page already mapped but fault again!");
    }

	// 合法合同且尚未映射，开始分配物理页
    // mapping_v2p 内部会完成 palloc 物理页 + 修改位图 + 建立页表映射
    mapping_v2p(PF_USER, page_vaddr);


	// 根据合同内容初始化物理页数据
    if (vma->vma_inode != NULL) {
        // 有文件的映射 (代码段、数据段、BSS)
        uint32_t offset_in_vma = page_vaddr - vma->vma_start;
        
        // 先全页清零，保证了 BSS 区域和文件末端对齐部分的正确性
        memset((void*)page_vaddr, 0, PG_SIZE);

        // 如果故障点在文件有效长度内，则读取磁盘
        if (offset_in_vma < vma->vma_filesz) {
            uint32_t read_size = PG_SIZE;
            // 最后一页可能不满 4KB
            if (offset_in_vma + PG_SIZE > vma->vma_filesz) {
                read_size = vma->vma_filesz - offset_in_vma;
            }
            
            // 物理偏移 = 合同起始偏移 + 块内偏移
            inode_read_data(vma->vma_inode, 
                            vma->vma_pgoff + offset_in_vma, 
                            (void*)page_vaddr, 
                            read_size);
        }
    } else {
        // 匿名映射 (栈、堆 brk 区域) 
        // 按照规定，新分配的匿名页必须初始化为全 0
		// 这里可以直接自动实现我们的栈扩容逻辑
        memset((void*)page_vaddr, 0, PG_SIZE);
    }

	intr_set_status(_old);
    return;

segmentation_fault:
    if (cur->pgdir != NULL) {
        printk("PID %d (%s) Segmentation Fault at %x\n", cur->pid, cur->name, vaddr);
        send_signal(cur, SIGSEGV);
    } else {
		// 内核进程不存在懒加载，直接报错
		printk("Kernel Page Fault at %x\n", vaddr);
        PANIC("Kernel Page Fault");
    }
    intr_set_status(_old);
}

// 触发写保护错误了，调用此函数，将相应的数据段的数据拷贝给触发写错误的进程
static void do_copy_on_write(uint32_t vaddr, uint32_t* pte, uint32_t old_pa) {
    // 分配新页（用户池）
    void* new_pa = palloc(&user_pool);
    if (new_pa == NULL) {
        PANIC("COW: No memory for new physical page.");
    }

    // 建立临时映射以进行拷贝
    // 使用 pte_ptr 获取中转页的 PTE 指针
    uint32_t* temp_pte = pte_ptr(K_TEMP_PAGE_VADDR);

	if ((*temp_pte & PG_P_1)) {
        PANIC("COW: K_TEMP_PAGE_VADDR is already in use! Collision detected.");
    }
    
    // 将新分配的物理页映射到中转虚拟地址，给予内核读写权限
	// PG_US_U 是用户的读写权限，我们最好不要给用户读写权限
    *temp_pte = (uint32_t)new_pa | PG_P_1 | PG_RW_W;
    
    // 必须刷新中转页的 TLB，否则可能写到旧映射的物理页里去（比如上一次调用do_copy_on_write所映射的物理页）
    asm volatile ("invlpg %0" : : "m" (*(char*)K_TEMP_PAGE_VADDR) : "memory");

    // 执行物理内存数据的搬运
    // 源地址：故障发生的虚拟页起始地址 (vaddr & 0xfffff000)
    // 目的地址：中转虚拟地址
    memcpy((void*)K_TEMP_PAGE_VADDR, (void*)(vaddr & 0xfffff000), PG_SIZE);

    // 更新原虚拟地址的映射关系
    // 现在 PTE 指向新物理页，并开启 PG_RW_W 写权限
    *pte = (uint32_t)new_pa | PG_P_1 | PG_RW_W | PG_US_U;
    pfree(old_pa);

    // 刷新出错虚拟地址的 TLB
    // 使 CPU 意识到该地址现在已经是新物理页且可写了
    asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
	// 拷贝完毕后，清理临时映射，一遍下次相同的操作能成功
	*temp_pte = 0; 
    asm volatile ("invlpg %0" : : "m" (*(char*)K_TEMP_PAGE_VADDR) : "memory");
}

// 写时复制（COW）：这是合法的，内核分配新页并映射，然后直接 ret 返回用户态继续执行。
// 非法写入：比如用户程序试图修改只读的代码段（.text）。
void write_protect(uint32_t err_code, void* err_vaddr) {
	enum intr_status _old =  intr_disable();
	
    struct task_struct* cur = get_running_task_struct();

    // 内核触发写保护：直接 PANIC，因为内核不参与 COW
    if (cur->pgdir == NULL || (uint32_t)err_vaddr >= 0xC0000000) {
        PANIC("Kernel Write Protection Error!");
    }

	uint32_t vaddr = (uint32_t)err_vaddr;
    
	// 先判断当前发送只读页错误的地址是不是位于数据段
	// 要是位于代码段的话那么本来也就不让改写
	// 直接发信号终止程序

	// 现在我们已经开始接入 vma 了
	// 在后续的实现中我们需要改进成使用 vma->flags & PF_W
	// 来判断是否为代码段
	if (vaddr < cur->end_code) {
        printk("PID %d (%s) attempt to write Read-Only Segment at %x\n", cur->pid, cur->name, vaddr);
        send_signal(cur, SIGSEGV);
		intr_set_status(_old);
        return;
    }

	uint32_t* pte = pte_ptr(vaddr);
    uint32_t pa = *pte & 0xfffff000;
	// 取出页目录表中的索引和页表中的索引
    uint32_t page_idx = pa >> 12;

    // COW 处理
    if (mem_map[page_idx] > 1) {
        // 确实有多个进程共享，执行拷贝
        do_copy_on_write(vaddr, pte, pa);
    } else if (mem_map[page_idx] == 1) {
        // 只有一个人用了，直接恢复写权限，不用额外拷贝了
        *pte |= PG_RW_W;
		// 刷新页目录项
        asm volatile ("invlpg %0" : : "m" (*(char*)vaddr));
    } else {
        PANIC("write_protect: mem_map counter error!");
    }

	intr_set_status(_old);
}


void sys_test(){

}

// 检查虚拟地址池中从 vaddr 开始的 pg_cnt 个页是否都是空闲的
static bool vaddr_range_is_free(struct virtual_addr* vaddr_pool, uint32_t vaddr, uint32_t pg_cnt) {
    // 计算该虚拟地址在位图中的起始偏移（第几个 bit）
    uint32_t bit_idx_start = (vaddr - vaddr_pool->vaddr_start) / PG_SIZE;
    
    // 逐页检查位图状态
	uint32_t i;
    for (i = 0; i < pg_cnt; i++) {
        // 如果 bitmap_bit_check 返回 true，说明该位是 1，即已被占用
        if (bitmap_bit_check(&vaddr_pool->vaddr_bitmap, bit_idx_start + i)) {
            return false; 
        }
    }
    return true;
}

// 修改进程的堆顶边界 (brk) 
uint32_t sys_brk(uint32_t new_brk) {
    struct task_struct* cur = get_running_task_struct();
	// 若new_brk == 0，表示是查询当前堆顶
    if (new_brk == 0) return cur->brk;
	// 非法缩减，将堆顶缩到数据区了，缩减失败，同样返回当前堆顶
    if (new_brk < cur->end_data) return cur->brk;

    uint32_t old_brk_page = cur->brk & 0xfffff000;
    uint32_t new_brk_page = new_brk & 0xfffff000;

    if (new_brk > cur->brk) {
        if (new_brk_page > old_brk_page) {
            // 安全检查, 检查 [old_brk_page + PG_SIZE, new_brk_page] 范围在位图中是否可用
            // 这一步可以防止堆撞上栈或 mmap 区域
			// 需要新分配的起始虚拟地址（当前堆顶所在页的下一页）
            uint32_t start_vaddr = old_brk_page + PG_SIZE;
            // 需要分配的总页数
            uint32_t pg_cnt = (new_brk_page - old_brk_page) / PG_SIZE;
            if (!vaddr_range_is_free(&cur->userprog_vaddr, start_vaddr, pg_cnt)) {
                printk("sys_brk: vaddr collision! range [%x, %x] is occupied.\n", 
                        start_vaddr, new_brk_page);
				PANIC("sys_brk: vaddr_range_is_free failed!\n");
                return cur->brk; // 发现前方有 mmap 或其他映射，扩张失败
            }
            
            //  分配
            uint32_t vaddr = start_vaddr;
            while (vaddr <= new_brk_page) {
                if (mapping_v2p(PF_USER, vaddr) == NULL) {
                    // 这里的错误处理：如果分配了一半失败了，理论上应该回滚，
                    // 但简易内核可以记录当前分配到的位置
					PANIC("sys_brk: mapping_v2p failed!\n");
                    return cur->brk; 
                }
                vaddr += PG_SIZE;
            }
        }
    } else if (new_brk < cur->brk) {
        // 缩减时的安全检查：不能释放包含 end_data 的那一页
        uint32_t data_end_page = cur->end_data & 0xfffff000;
        
        if (new_brk_page < old_brk_page) {
            uint32_t vaddr = old_brk_page;
            // 只有当 vaddr 超过了数据段所在的页，才允许释放
            while (vaddr > new_brk_page && vaddr > data_end_page) {
                mfree_page(PF_USER, (void*)vaddr, 1);
                vaddr -= PG_SIZE;
            }
        }
    }

    cur->brk = new_brk;
    return cur->brk;
}