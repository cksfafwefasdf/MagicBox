#include <memory.h>
#include <print.h>
#include <debug.h>
#include <stdint.h>
#include <string.h>
#include <global.h>
#include <sync.h>
#include <interrupt.h>
#include <stdio-kernel.h>
#include <bitmap.h>
#include <thread.h>
#include <process.h>
#include <stdint.h>
#include <vma.h>
#include <buddy.h>

struct arena{
	// point to the mem_block which is related to this arena
	struct mem_block_desc* desc;
	// when large is true, cnt is the number of page frames
	// for example, when malloc 5000KB, the cnt is 2
	// otherwise it is the number of the free mem_block
	uint32_t cnt;
	bool large; // when malloc above 1024Bytes, large is true
    uint8_t pad[3]; // 凑够12字节
}__attribute__((packed));;

// uint8_t* mem_map = NULL;

// 替换原本实现中的 uint8_t* mem_map
// 由于每一个物理页会对应一个 page 结构体，因此global_pages的大小和物理页有关
struct page* global_pages; 

struct buddy_pool kernel_pool,user_pool;

struct mem_block_desc k_block_descs[DESC_TYPE_CNT];

struct virtual_addr kernel_vaddr;

uint32_t mem_bytes_total = 0;
uint32_t total_pages = 0;

uint32_t kernel_heap_start = 0; // 在 mem_pool_init 中动态赋值

int32_t inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count);
static struct vm_area* find_vma_condition(struct task_struct* task, uint32_t flags);

// 这是一个内部辅助函数，用于在 mem_pool_init 这种极早期阶段手动绑定物理地址和虚拟地址的映射
static void early_map(uint32_t vaddr, uint32_t paddr) {
    uint32_t* pde = pde_ptr(vaddr);
    uint32_t* pte = pte_ptr(vaddr);

    // 如果 PDE 不存在，说明这 4MB 范围的页表还没分配
    if (!(*pde & 0x1)) {
        // 在这种极早期，我们还没法用 palloc，直接硬点一块物理内存给页表使用
        PANIC("Early PDE not exist! Please check your Loader setup_page.");
    }

    // 填写 PTE
    *pte = paddr | PG_P_1 | PG_RW_W | PG_US_U;

    // 刷新 TLB
    asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
}	

static void mem_pool_init(uint32_t all_mem) {
    put_str("mem_pool init start\n");

    // 计算物理内存布局 (避开 1MB, 页表, global_pages)
	// [total page table size] = [PDT itself](1pg) + [item-0 and item-768 point same place,low 4MB](1pg)+[item-769](1pg)+...+[item-1022](1pg)
	// = 256 pg
	// page-table use 256 pages by itself
    uint32_t page_table_size = PG_SIZE * 256;

	// 0x100000 是内核的数据大小
	// page_table_size 是 1MB
	// base_used_mem 是 2MB
    uint32_t base_used_mem = KERNEL_RESERVED_SPACE + page_table_size;
    
    total_pages = all_mem / PG_SIZE;
    uint32_t global_pages_size = total_pages * sizeof(struct page);
    
	// 按理来说，global_pages 会被放到物理地址的低 2MB 之后
    global_pages = (struct page*)(KERNEL_VADDR_START + base_used_mem);


	// 由于我们直接进行了低端内存映射，因此低512MB的内存都是直接被建立好映射的
	// 不用去额外建立映射了

	// 在 memset 之前手动建立映射
    uint32_t cur_vaddr = (uint32_t)global_pages;
    uint32_t cur_paddr = base_used_mem; // 物理地址也从 2MB 开始
    uint32_t mapped_size = 0;

    while (mapped_size < global_pages_size) {
        early_map(cur_vaddr, cur_paddr);
        cur_vaddr += PG_SIZE;
        cur_paddr += PG_SIZE;
        mapped_size += PG_SIZE;
    }

    memset(global_pages, 0, global_pages_size);

    // 计算真正可供伙伴系统使用的物理内存起始点
    uint32_t real_phy_start = base_used_mem + global_pages_size;
    real_phy_start = (real_phy_start + PG_SIZE - 1) & 0xfffff000; // 对齐

    uint32_t total_free_byte = all_mem - real_phy_start;
    uint32_t kernel_pool_size = total_free_byte / 2;
    uint32_t user_pool_size = total_free_byte - kernel_pool_size;

    // 初始化物理伙伴池
    buddy_init(&kernel_pool, real_phy_start, kernel_pool_size, global_pages);
    buddy_init(&user_pool, real_phy_start + kernel_pool_size, user_pool_size, global_pages);

    // 初始化内核虚拟地址池 (必须保留)，因为我们的虚拟地址目前还是基于位图产生的
	// 后期要改进成基于brk
    // 此时 MEM_BITMAP_BASE 只需要存放一个位图了（内核虚拟地址位图，这样的话我们支持的内存空间也就能大很多了）
    kernel_vaddr.vaddr_bitmap.bits = (void*)MEM_BITMAP_BASE;
    // 这里的长度代表内核虚拟地址空间的覆盖范围，给个 32MB 的范围对应的位图也就 1KB
	// 即使我们的物理内存有 4GB，那么它所占用的位图大小为 4GB / (4096 B/page) / (8 page/byte) = 32 页
	// 我们在 make_main_thread 中改进了内核PCB的位置，我们先前的实现中将内核PCB放到了内存低端的1MB中
	// 并在loader中将内核栈硬编码成了 0xc009f000，且在之后的操作中一直继承了这个栈
	// 现在我们改进了这个分配方法，将内核的PCB动态分配到了内核堆空间中，使它和其他的内核线程对等
	// 这样就更灵活了，这样一来0xc009a000 ~ 0xc0100000 的这部分空间就完全空闲下来了

    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = (kernel_pool_size / PG_SIZE) / 8; 
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    // 内核堆起始位置，紧跟在 global_pages 之后
	// 动态计算堆起始地址
    // 堆必须在 global_pages 数组结束之后开始，以防虚拟地址冲突
    kernel_heap_start = (uint32_t)global_pages + global_pages_size;
    kernel_heap_start = (kernel_heap_start + PG_SIZE - 1) & 0xfffff000; // 对齐到页

    kernel_vaddr.vaddr_start = kernel_heap_start;

	put_str("Kernel pool range: "); put_int(kernel_pool.phy_addr_start);
	put_str(" - "); put_int(kernel_pool.phy_addr_start + kernel_pool_size);
	put_str("\n");

    put_str("mem_pool_init done\n");
}

void mem_init(void){
	put_str("mem_init start\n");
	// 在内核页表中，0xc0000000 开始的 1MB 和 0x0 开始的 1MB 映射的是同一个区域
	// 所以可以直接用 SYS_MEM_SIZE_PTR
	mem_bytes_total = *((uint32_t*)(SYS_MEM_SIZE_PTR));

	mem_pool_init(mem_bytes_total);
	block_desc_init(k_block_descs);
	put_str("mem_init done\n");
}


// allocate [pg_cnt] pages from [pool_flags] mem-pool
static void* vaddr_alloc(enum pool_flags pf, uint32_t pg_cnt, bool force_mmap) {
    enum intr_status old = intr_disable();
    uint32_t vaddr_start = 0;

    if (pf == PF_KERNEL) {
        // 内核态，维持原状，继续使用全局位图
        int bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if (bit_idx_start == -1) {
            intr_set_status(old);
            return NULL;
        }
        uint32_t cnt = 0;
        while (cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt, 1);
            cnt++;
        }
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
		intr_set_status(old);
		return (void*)vaddr_start;

    } else {
        struct task_struct* cur = get_running_task_struct();
        uint32_t size = pg_cnt * PG_SIZE;

        // 如果不是强制使用 mmap，先尝试推高堆顶 (brk)
        if (!force_mmap) {
            uint32_t old_brk = cur->brk;
            uint32_t new_brk = old_brk + size;
            uint32_t actual_brk = sys_brk(new_brk);
            if (actual_brk >= new_brk) {
                // sys_brk 成功推高了堆顶，返回旧堆顶作为分配的起始虚拟地址
                intr_set_status(old);
                return (void*)old_brk;
            }
        }
        

        // 如果是超大内存，或者 brk 失败了，走 mmap 区域 (Gap 搜索)
        vaddr_start = vma_find_gap(cur, pg_cnt);
        
        if (vaddr_start != 0) {
            // 把找出来的 Gap 注册到 VMA 链表里
            // 这样 find_vma 才能搜到它，页错误处理才能正常工作
            vaddr_start = PAGE_ALIGN_UP(vaddr_start);
            add_vma(cur, vaddr_start, vaddr_start + size, 0, NULL, VM_READ | VM_WRITE | VM_ANON|VM_GROWSUP, 0);
            
            intr_set_status(old);
            return (void*)vaddr_start;
        }
        intr_set_status(old);
        return NULL; // 彻底没空间了
    }
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
void* palloc(struct buddy_pool* m_pool) {
    // 关中断保证原子性（因为涉及 pool 中空闲链表的修改）
    enum intr_status old = intr_disable();

    // 调用伙伴系统的核心分配函数，申请 1 个物理页 (order 0)
    // 伙伴系统内部会处理 lock
    struct page* pg = palloc_pages(m_pool, 0);

    if (pg == NULL) {
        intr_set_status(old);
        return NULL;
    }

    // 设置引用计数，新分配的页，引用计数初始化为 1
    pg->ref_count = 1;

    // 将 struct page 转换为物理地址返回
    uint32_t page_phyaddr = PAGE_TO_ADDR(m_pool,pg);

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
		if (*pte & 0x00000001) {
			put_str("Conflict vaddr: ");put_int(vaddr);put_str("pte val: ");put_int(*pte);put_str("\n");
		}
		ASSERT(!(*pte & 0x00000001));
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

// 根据请求的页面数计算需要的伙伴系统 order
// 主要用于查找最大的，但是小于pg_cnt的一个2^order
// 例如对于 31，会找到 2^4=16 ，其中order为4
static uint32_t pg_cnt_to_order(uint32_t pg_cnt) {
    uint32_t order = 0;
    while ((1U << order) < pg_cnt) {
        order++;
    }
    return order;
}

// 由于palloc_pages只能申请2的幂次块，因此该函数用于处理申请129这种非2的幂次的情况
// 对于 129页，我们会先申请一个256页的块
// 然后循环依次将剩下的127块释放
static struct page* palloc_pages_exact(struct buddy_pool* bpool, uint32_t pg_cnt) {
    uint32_t order = pg_cnt_to_order(pg_cnt);
    
    // 申请 2^order 个物理页
    struct page* first_pg = palloc_pages(bpool, order);
    if (first_pg == NULL) return NULL;

	// 先全部锁死，不准合并，之后再精准释放
	for (uint32_t i = 0; i < (1U << order); i++) {
    	(first_pg + i)->flags |= 1; 
	}

    // 将多余的物理页 [pg_cnt, 2^order) 释放回伙伴系统
    // 这里应该是安全的，因为伙伴系统会自动合并这些小块
    for (uint32_t i = pg_cnt; i < (1U << order); i++) {
		struct page* target = first_pg + i;
		target->flags &= ~1; // 只解锁这一页
		target->order = 0; // 归还前必须重置
        // 释放时必须指定 order 为 0，让它一页页合并回去
        pfree_pages(bpool, target, 0);
    }

    return first_pg;
}

// 分配物理上连续的若干页，虚拟地址也连续
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
    ASSERT(pg_cnt > 0);
    
    // 统一申请虚拟地址 (申请多少给多少)
    // 内核态是不管 force_mmap 标志的，他还是用位图来分配
    // 因此这个标志主要还是给用户态用，我们沿用用户态的标准，大于128KB将其置为true
    bool force_mmap = pg_cnt>=32?true:false;
    void* vaddr_start = vaddr_alloc(pf, pg_cnt, force_mmap);
    if (vaddr_start == NULL) return NULL;

    struct buddy_pool* mem_pool = (pf & PF_KERNEL) ? &kernel_pool : &user_pool;

    // 统一申请物理地址，这些物理地址是物理上连续的
    struct page* first_pg = palloc_pages_exact(mem_pool, pg_cnt);
    if (first_pg == NULL) {
        // 这里可以做 vaddr_remove 的回滚
		// 但是我们目前先直接PANIC
		PANIC("malloc_page: first_pg == NULL");
        return NULL;
    }

    // 统一建立页表映射
    uint32_t vaddr = (uint32_t)vaddr_start;
    uint32_t paddr = PAGE_TO_ADDR(mem_pool, first_pg);
    
    for (uint32_t i = 0; i < pg_cnt; i++) {
        page_table_add((void*)vaddr, (void*)paddr);
        // 设置引用计数，每一页都应该为 1，因为它们现在被映射了
        (first_pg + i)->ref_count = 1;
        
        vaddr += PG_SIZE;
        paddr += PG_SIZE; 
    }

	// put_str("Mapped PADDR: "); put_int(paddr); put_str("\n");
	
	// put_str("vaddr_start: "); put_int(vaddr_start);put_str("\n");

    return vaddr_start;
}

// 分配虚拟地址连续，但是物理地址不连续的若干页
static void* vmalloc_page(enum pool_flags pf, uint32_t pg_cnt,bool force_mmap) {
    // 获取虚拟地址（内核走位图/固定堆，用户走 VMA Gap）
    void* vaddr_start = vaddr_alloc(pf, pg_cnt,force_mmap);
    if (vaddr_start == NULL) return NULL;

    if (pf == PF_KERNEL) { // 内核态，必须立即分配物理页，内核不容许 Page Fault（除非是置换逻辑）
        uint32_t vaddr = (uint32_t)vaddr_start;
        uint32_t cnt = pg_cnt;
        while (cnt-- > 0) {
            void* page_phyaddr = palloc(&kernel_pool);
            if (page_phyaddr == NULL) return NULL; // 实际中内核分配失败通常是致命的
            page_table_add((void*)vaddr, page_phyaddr);
            vaddr += PG_SIZE;
        }
    } else {
        // 用户态，既然 vaddr_alloc 已经 add_vma 了，我们直接返回
        // 物理页？不存在的。等用户写数据时，swap_page 会来补齐。
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
// 引入vma后，此函数只管分物理页和挂页表。
// 我们靠 vma_find_gap 函数来保证虚拟地址不冲突
void* mapping_v2p(enum pool_flags pf,uint32_t vaddr){
	struct buddy_pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;
	lock_acquire(&mem_pool->lock);
	struct task_struct* cur = get_running_task_struct();
	int32_t bit_idx = -1;

	// 内核态下，我们仍然先保留位图来管理虚拟地址空间
	// 用户态下我们使用vma来管理
	if(cur->pgdir==NULL&&pf==PF_KERNEL){
		// if cur is a kernel thread
		bit_idx = (vaddr-kernel_vaddr.vaddr_start)/PG_SIZE;
		ASSERT(bit_idx>0);
		bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx,1);
	}
	// 由于我们在 add_vma 时已经确认了这块地的合法性
	// 因此此处不用再去操作相关的东西了，只用绑定物理地址和虚拟地址就行
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
	struct buddy_pool* mem_pool;
	struct mem_block_desc* descs;
	struct task_struct* cur_thread = get_running_task_struct();
    // 阈值128KB。Linux 常用这个值作为 brk 和 mmap 的分界线
    const uint32_t MMAP_THRESHOLD = 128 * 1024;

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

        bool force_mmap = (PF == PF_USER && size > MMAP_THRESHOLD);

		a = vmalloc_page(PF,page_cnt,force_mmap);

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
			
            // 小空间默认走brk逻辑
			a = vmalloc_page(PF,1,false);
			
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
void pfree(uint32_t pg_phy_addr) {
    struct page* pg = ADDR_TO_PAGE(global_pages,pg_phy_addr);

    // 确保不是在释放一个已经空闲的页
    ASSERT(pg->ref_count > 0);

    enum intr_status old = intr_disable();

    // 递减引用计数
    pg->ref_count--;

    // 判断是否需要归还给伙伴系统
    if (pg->ref_count == 0) {
        // 这里的 pool 判断逻辑之前的一致
        struct buddy_pool* m_pool = (pg_phy_addr >= user_pool.phy_addr_start) ? &user_pool : &kernel_pool;
        
        // 调用伙伴系统的释放逻辑，尝试合并
        pfree_pages(m_pool, pg, 0);
    }

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
void vaddr_remove(enum pool_flags pf,void* _vaddr,uint32_t pg_cnt){
	enum intr_status old = intr_disable();
	uint32_t bit_idx_start = 0,vaddr = (uint32_t)_vaddr,cnt=0;
	if(pf==PF_KERNEL){
		bit_idx_start = (vaddr-kernel_vaddr.vaddr_start)/PG_SIZE;
		while(cnt<pg_cnt){
			bitmap_set(&kernel_vaddr.vaddr_bitmap,bit_idx_start+cnt++,0);
		}
	}else{
		// 用户态, 使用 VMA 逻辑
        struct task_struct* cur = get_running_task_struct();
        uint32_t start = (uint32_t)_vaddr;
        uint32_t end = start + pg_cnt * PG_SIZE;

        struct vm_area* vma = find_vma(cur, start);
        if (vma == NULL) return;

        // 若完全覆盖，此时直接移除vma
        if (start == vma->vma_start && end == vma->vma_end) {
            remove_vma(vma);
        } else if (start == vma->vma_start && end < vma->vma_end) { // 若释放开头，则起点后移
            vma->vma_start = end;
            if (vma->vma_inode) {
                vma->vma_pgoff += pg_cnt;
            }
        } else if (start > vma->vma_start && end == vma->vma_end) { // 若释放末尾，则终点前移
            vma->vma_end = start;
        } else if (start > vma->vma_start && end < vma->vma_end) { // 若挖洞（释放中间部分），则分裂
            // 先在 end 处切一刀，分成 [vma_start, end] 和 [end, vma_end]
            struct vm_area* next_part = vma_split(vma, end);
            if (next_part == NULL) {
                PANIC("vaddr_remove: split failed, out of memory!");
            }
            // 此时 vma 变成了 [vma_start, end]，现在把它变成 [vma_start, start]
            // 这样中间 [start, end] 这一段就自然被“挖除”了
            vma->vma_end = start;
        }
        // 如果释放的虚拟内存位于堆顶区域，同步更新brk
        if (end >= cur->brk && start < cur->brk) {
            cur->brk = start;
        }
	}
	intr_set_status(old);
}

// 仅释放物理页映射，保留虚拟地址空间 (不调用 vaddr_remove) 
// 适用于：堆(brk)中 Arena 的释放，保持堆的连续性
static void mfree_physical_pages(void* _vaddr, uint32_t pg_cnt) {
    uint32_t vaddr = (uint32_t)_vaddr;
    
    for (uint32_t i = 0; i < pg_cnt; i++) {
        uint32_t cur_vaddr = vaddr + i * PG_SIZE;
        
        // 检查页表
        uint32_t* pte = pte_ptr(cur_vaddr);
        // 如果 P 位为 1，说明已经建立了物理映射，需要回收
        if (pte && (*pte & PG_P_1)) { 
            uint32_t pg_phy_addr = addr_v2p(cur_vaddr);
            
            // 释放物理页返回物理内存池
            pfree(pg_phy_addr);
            
            // 清除页表项 (PTE) 并刷新 TLB
            // 以便后续触发缺页操作重新分配
            page_table_pte_remove(cur_vaddr);
        }
    }
}

// 释放物理页映射，并销毁虚拟地址空间记录
// 适用于：大内存(mmap)释放，或者内核固定分配的释放
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
    // 先回收物理部分
    mfree_physical_pages(_vaddr, pg_cnt);
    
    // 再回收虚拟部分（位图或 VMA）
    vaddr_remove(pf, _vaddr, pg_cnt);
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
// 用户通常的malloc和free操作是不会将堆的vma挖出洞来的，只是会回收物理内存而已
// 除非用户手动调用munmap等操作，不然的话即使释放到堆vma中间的部分，他也只是物理清空
// 虚拟不收缩，直到重新访问时触发页错误来分配
void do_free(void* ptr,enum pool_flags PF){
    // put_str("ptr: ");put_int(ptr);put_str("\n");
	
    ASSERT(ptr!=NULL);
	if(ptr==NULL) return;

    struct buddy_pool* mem_pool = (PF == PF_KERNEL) ? &kernel_pool : &user_pool;

    lock_acquire(&mem_pool->lock);
    
    struct mem_block* b = ptr;
    struct arena* a = block2arena(b);

    // 找到进程唯一的堆 VMA
    // 它是通过 execv 初始化，且由 sys_brk 负责伸缩的那个区域
    // 它是第一个被创建的，具有VM_READ|VM_WRITE|VM_GROWSUP|VM_ANON属性的区域
    struct task_struct* cur = get_running_task_struct();
    struct vm_area* heap_vma = find_vma_condition(cur, VM_READ|VM_WRITE|VM_GROWSUP|VM_ANON);

    if(heap_vma==NULL&&PF == PF_USER){
        PANIC("do_free fail to find heap_vma!\n");
    }

    bool is_in_heap = false;
    // 判定当前释放的 Arena 是否落在堆范围内
    // 只要起始地址落入即可，因为 Arena 是连续分配的
    uint32_t addr = (uint32_t)a;
    if(PF!=PF_KERNEL){
        is_in_heap = (addr >= heap_vma->vma_start && addr < heap_vma->vma_end);
    }
    

    uint32_t vaddr = (uint32_t)ptr;
    // 物理检查，不依赖内存读取，直接查页表
    uint32_t* pte = pte_ptr(vaddr);
    
    // 如果 PTE 根本不存在，或者 P 位为 0
    if (pte == NULL || !(*pte & PG_P_1)) {
        // 只有当这个地址连 VMA 合同都没有的时候，才是真正的非法释放
        struct vm_area* vma = find_vma(get_running_task_struct(), vaddr);
        if (vma == NULL) {
            printk("CRITICAL: Invalid Free at %x (No VMA)!\n", vaddr);
            PANIC("Invalid free!");
        } else {
            // 否则只是惰性分配导致的空页面释放
            // 由于我们只在触发页错误时采取分配物理内存
            // 如果一个页被逻辑上分配了虚拟内存后，但是从来没有去访问过
            // 那么这个页是不会有对应的物理内存的，如果对这样的页进行释放的话我们直接返回就行
            // printk("INFO: %x was touched before, but P-bit is gone. Page-table value: %x\n", vaddr, (pte ? *pte : 0));
#ifdef DEBUG_PG_FAULT
            printk("DEBUG: Pointer %x has no physical page. (Page already recycled)\n", vaddr);
#endif
            return; 
        }
    }

    ASSERT(a->large==0||a->large==1);

    if(a->desc==NULL&&a->large==true){
        if (is_in_heap) {
            // 堆内大对象。只回收物理内存，保留虚拟占位，防止堆穿孔。
            mfree_physical_pages(a, a->cnt);
        } else {
            // 独立映射区对象。物理和虚拟记录（VMA）一并销毁。
            mfree_page(PF, a, a->cnt);
        }
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
            
            if(PF==PF_KERNEL){
                // 对于内核来说，我们仍然使用位图来管理空间，因此还是沿用原本的 mfree_page
                // 不要制造缺页
                mfree_page(PF,a,1);
            }else{
                // 物理回收，节省物理内存
                // 虚拟内存保留，维持堆的连续性，不调用 vaddr_remove
                // 后续如果再被分配到，可以通过缺页中断来重新进行物理页的分配
                mfree_physical_pages(a,1);
            }
        }
    }
    lock_release(&mem_pool->lock);
	
}

void sys_free_mem(){
	uint32_t user_total = mem_bytes_total/2;
	uint32_t kernel_total = mem_bytes_total/2;
	printk("kernel_total: %d\n",user_total);
	printk("user_total: %d\n",kernel_total);
}

void sys_test(){

}

// 查找带有特定权限的vma，若有多个，返回第一个
static struct vm_area* find_vma_condition(struct task_struct* task, uint32_t flags){
    struct vm_area* heap_vma = NULL;
    struct dlist_elem* e = task->vma_list.head.next;
    while (e != &task->vma_list.tail) {
        struct vm_area* v = member_to_entry(struct vm_area, vma_tag, e);
        // 堆的特征：起始地址对齐，且没有关联文件 inode
        // 最好不要用find_vma来找，因为
        if (v->vma_flags == (flags)) {
            heap_vma = v;
            break;
        }
        e = e->next;
    }
    return heap_vma;
}

// 修改进程的堆顶边界 (brk) 
// 堆顶边界可以以任意值扩展
// 但是实际的物理映射的建立和销毁是以页为单位的
// 也就是说假如没有页对齐的话，用户即使只向上推1B，我们也会建立一个页的物理页来对齐进行映射
// 在此之后再怎么推，只要不超过这个页，那么这个映射就不会被修改，而只是单纯的推高brk和vma的end的数值
// 只有一个页的虚拟地址被完全回收时，我们才会将相应的物理页销毁，否则即使只剩1B我们也得留着
uint32_t sys_brk(uint32_t new_brk) {
    struct task_struct* cur = get_running_task_struct();
    if (new_brk == 0) return cur->brk;
    if (new_brk < cur->end_data) return cur->brk;

    struct vm_area* heap_vma = find_vma_condition(cur, VM_READ|VM_WRITE|VM_GROWSUP|VM_ANON);
    ASSERT(heap_vma != NULL);

    uint32_t old_brk_aligned = PAGE_ALIGN_UP(cur->brk);
    uint32_t new_brk_aligned = PAGE_ALIGN_UP(new_brk);

    //  缩小堆
    if (new_brk < cur->brk) {
        // 只有当对齐后的边界发生回退，才需要真正“收地”
        if (new_brk_aligned < old_brk_aligned) {
            // 释放不再需要的物理页，并清理页表映射
            // 释放范围是 [new_brk_aligned, old_brk_aligned]
            uint32_t release_pg_cnt = (old_brk_aligned - new_brk_aligned) / PG_SIZE;
            mfree_page(PF_USER, (void*)new_brk_aligned, release_pg_cnt);
            
            // 更新 VMA 边界
            heap_vma->vma_end = new_brk_aligned;
        }
        cur->brk = new_brk; // 记录用户的精确 brk
        return cur->brk;
    }

    // 扩大堆
    // 如果对齐后的边界没变，说明还在同一页内，直接更新 brk 即可
    if (new_brk_aligned <= old_brk_aligned) {
        cur->brk = new_brk;
        return cur->brk;
    }

    // 检查是否撞上后续 VMA
    struct dlist_elem* next_elem = heap_vma->vma_tag.next;
    if (next_elem != &cur->vma_list.tail) {
        struct vm_area* next_vma = member_to_entry(struct vm_area, vma_tag, next_elem);
        if (new_brk_aligned > next_vma->vma_start) {
            return cur->brk; // 空间不足
        }
    }

    if (new_brk_aligned >= USER_STACK_BASE) return cur->brk;

    // 更新 VMA 边界（画饼，不实际分配内容，直到发生页错误，让swap_page来分配）
    heap_vma->vma_end = new_brk_aligned;
    cur->brk = new_brk;

    return cur->brk;
}