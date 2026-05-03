#include <memory.h>
#include <vgacon.h>
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
#include <file.h>
#include <file_table.h>

// uint8_t* mem_map = NULL;

// 替换原本实现中的 uint8_t* mem_map
// 由于每一个物理页会对应一个 page 结构体，因此global_pages的大小和物理页有关
struct page* global_pages; 

struct buddy_pool kernel_pool,user_pool;

struct mem_block_desc k_block_descs[DESC_TYPE_CNT];
static struct lock kmap_lock;
static uint32_t kmap_slots[KMAP_SLOT_CNT];
static uint32_t kernel_direct_map_limit = 0;

uint32_t mem_bytes_total = 0;
uint32_t total_pages = 0;

uint32_t kernel_heap_start = 0; // 在 mem_pool_init 中动态赋值

int32_t inode_read_data(struct inode* inode, uint32_t offset, void* buf, uint32_t count);
static struct vm_area* find_heap_vma(struct task_struct* task);
static bool is_kernel_vaddr(uint32_t vaddr);
static void direct_map_lowmem_range(uint32_t start_paddr, uint32_t end_paddr);
static void* direct_map_ptr(uint32_t paddr);
static void* do_alloc(uint32_t size);
static void do_free(void* ptr);
static uint32_t prot_to_vm_flags(uint32_t prot, bool anon);
static struct vm_area* find_covering_or_next_vma(struct task_struct* task, uint32_t vaddr);
static uint32_t do_mmap(struct task_struct* cur, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t offset);

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

    // 下对齐，防止将不存在的物理页也当作是可以映射的
    // 这么做最多也就浪费4095字节，问题不是太大
    kernel_direct_map_limit = all_mem < KERNEL_DIRECT_SIZE ? PAGE_ALIGN_DOWN(all_mem) : KERNEL_DIRECT_SIZE;
    // 先建立低端内存映射
    direct_map_lowmem_range(0, kernel_direct_map_limit);
    
	// 按理来说，global_pages 会被放到物理地址的低 2MB 之后
    global_pages = (struct page*)(KERNEL_VADDR_START + base_used_mem);
    // 由于我们在 direct_map_lowmem_range 中建立过低端内存映射了
    // 因为 global_pages 是一个必须要能被内核持久访问的对象 因此 global_pages 百分之百是在低端内存里的
    // 所以此处 memset 不应该会报错
    memset(global_pages, 0, global_pages_size);

    // 计算真正可供伙伴系统使用的物理内存起始点
    uint32_t real_phy_start = base_used_mem + global_pages_size;
    real_phy_start = (real_phy_start + PG_SIZE - 1) & 0xfffff000; // 对齐

    uint32_t total_free_byte = all_mem - real_phy_start;
    uint32_t lowmem_free_bytes = 0;
    if (kernel_direct_map_limit > real_phy_start) {
        lowmem_free_bytes = kernel_direct_map_limit - real_phy_start;
    }

    // 为了简单起见，我们先把空闲低端内存的一半给内核，然后剩下的所有空间都给用户
    // 我们现在的双内存池设计是有很多缺陷的，他在物理上将内存进行了隔离，虽然安全但是非常不灵活
    // 后期我们可以参考 Linux 的设计，不再区分用户内存池和内核内存池，而是统一用一套内存池来管理
    // 然后通过权限，用途之类的字段来对每个页进行区分
    uint32_t kernel_pool_size = PAGE_ALIGN_DOWN(lowmem_free_bytes / 2);
    uint32_t user_pool_size = PAGE_ALIGN_DOWN(total_free_byte - kernel_pool_size);

    if (kernel_pool_size == 0) {
        PANIC("mem_pool_init: no lowmem left for kernel_pool");
    }

    // 初始化物理伙伴池
    buddy_init(&kernel_pool, real_phy_start, kernel_pool_size, global_pages);
    buddy_init(&user_pool, real_phy_start + kernel_pool_size, user_pool_size, global_pages);

    lock_init(&kmap_lock);
    memset(kmap_slots, 0, sizeof(kmap_slots));

    kernel_heap_start = KERNEL_PAGE_OFFSET + real_phy_start;

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
        intr_set_status(old);
        PANIC("vaddr_alloc: PF_KERNEL should use direct mapping instead");
        return NULL;

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

// 在直接映射区，通过一个物理地址获得虚拟地址
static void* direct_map_ptr(uint32_t paddr) {
    ASSERT(paddr_is_lowmem(paddr));
    return (void*)(KERNEL_PAGE_OFFSET + paddr);
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
        printk("palloc: warning, palloc return NULL!\n");
        return NULL;
    }

    // 设置引用计数，新分配的页，引用计数初始化为 1
    pg->ref_count = 1;

    // 将 struct page 转换为物理地址返回
    uint32_t page_phyaddr = PAGE_TO_ADDR(m_pool,pg);

    intr_set_status(old);
    // printk("palloc: alloc paddr: 0x%x\n",page_phyaddr);
    return (void *)page_phyaddr;
}

// add relation between _vaddr and _page_phyaddr
static void page_table_add(void* _vaddr,void* _page_phyaddr){
	enum intr_status old = intr_disable();
	uint32_t vaddr = (uint32_t)_vaddr,page_phyaddr = (uint32_t)_page_phyaddr;
	uint32_t* pde = pde_ptr(vaddr);
	uint32_t* pte = pte_ptr(vaddr);
    uint32_t page_flags = PG_P_1 | PG_RW_W | (is_kernel_vaddr(vaddr) ? PG_US_S : PG_US_U);
	// the lowest bit is P bit
	// check if the page exists in the mem
	if(*pde&0x00000001){
		if (*pte & 0x00000001) {
			put_str("Conflict vaddr: ");put_int(vaddr);put_str("pte val: ");put_int(*pte);put_str("\n");
		}
		ASSERT(!(*pte & 0x00000001));
		if(!(*pte&0x00000001)){
			*pte = page_phyaddr | page_flags;
		}else{
			PANIC("Duplicate PTE");
			*pte = page_phyaddr | page_flags;
		}
	}else{
		uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);
		*pde = pde_phyaddr | page_flags;
		memset((void*)((int)pte&0xfffff000),0,PG_SIZE);

		ASSERT(!(*pte&0x00000001));
		*pte = page_phyaddr | page_flags;
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

    // 我们现在的设计中，内核空间的大小等于直接映射区大小的一半
    // 因此内核池中申请的内存一定要在直接映射区
    // 这么做还有一个原因就是，通常来说，调用这个函数的目的都是为PCB之类的数据结构进行内存分配
    // 这类结构必须要在一个能被内核随时都能访问到的区域中
    if (pf == PF_KERNEL) {
        struct page* first_pg = palloc_pages_exact(&kernel_pool, pg_cnt);
        if (first_pg == NULL) {
            PANIC("malloc_page: kernel lowmem exhausted");
        }

        uint32_t paddr = PAGE_TO_ADDR(&kernel_pool, first_pg);
        if (!paddr_is_lowmem(paddr) || !paddr_is_lowmem(paddr + (pg_cnt - 1) * PG_SIZE)) {
            PANIC("malloc_page: kernel allocation escaped direct-mapped lowmem");
        }

        for (uint32_t i = 0; i < pg_cnt; i++) {
            (first_pg + i)->ref_count = 1;
        }

        // 由于低端内存区的映射我们在init阶段就直接建立好了，因此我们现在不需要额外插入页表项了

        // direct_map_lowmem_range(paddr, paddr + pg_cnt * PG_SIZE);
        return direct_map_ptr(paddr);
    }
    
    // 统一申请虚拟地址 (申请多少给多少)
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
    if (pf == PF_KERNEL) {
        return malloc_page(PF_KERNEL, pg_cnt);
    }

    // 获取虚拟地址（内核走位图/固定堆，用户走 VMA Gap）
    void* vaddr_start = vaddr_alloc(pf, pg_cnt,force_mmap);
    if (vaddr_start == NULL) return NULL;

    {
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
// 这个函数目前只有 swap_page 函数会调用
// 内核空间目前我们全用直接映射，所以不需要在此处进一步操作了，不会调用这个函数建立映射
void* mapping_v2p(uint32_t vaddr ,uint32_t paddr){
	// struct buddy_pool* mem_pool = pf&PF_KERNEL?&kernel_pool:&user_pool;
    ASSERT((void*)vaddr!=NULL && (void*)paddr!=NULL)
    
	// 由于我们在 add_vma 时已经确认了这块地的合法性
	// 因此此处不用再去操作相关的东西了，只用绑定物理地址和虚拟地址就行
	// void *page_phyaddr = palloc(mem_pool);
	// if(page_phyaddr==NULL){
	// 	lock_release(&mem_pool->lock);
	// 	return NULL;
	// }
	page_table_add((void*)vaddr,(void*)paddr);

    struct page* pg = ADDR_TO_PAGE(global_pages ,(uint32_t) paddr);

    // 建立反向映射的必要信息
    // 否则置换算法不知道该去哪个页表里找这个物理页
    pg->first_vaddr = vaddr; // 置换需要知道修改哪个虚拟地址的 PTE
    pg->first_owner = get_running_task_struct(); // 置换需要知道属于哪个进程
    pg->ref_count = 1;

    lock_acquire(&user_pool.lock);
    ASSERT(!dlist_is_linked(&pg->activate_tag));
    // 挂在队尾，以便遵循最久未使用的原则
    dlist_push_back(&user_pool.activate_list,&pg->activate_tag);

	lock_release(&user_pool.lock);

	return (void*)vaddr;
}

// use vaddr to get paddr
uint32_t addr_v2p(uint32_t vaddr){
    // 直接映射区的内容的话直接减去3GB后返回
    if (vaddr_is_directmap(vaddr)) {
        uint32_t paddr = vaddr - KERNEL_PAGE_OFFSET;
        if (paddr < kernel_direct_map_limit) {
            return paddr;
        }
    }
    // 不是低端内存的话要查页表
	// all of the ptrs is vaddr
	// so pte is vaddr
	uint32_t* pte = pte_ptr(vaddr);
	// *pte is paddr
	// vaddr&0x00000fff to get offset in low 12bits
	return ((*pte&0xfffff000)+(vaddr&0x00000fff));
}

bool paddr_is_lowmem(uint32_t paddr) {
    return paddr < kernel_direct_map_limit;
}

bool vaddr_is_directmap(uint32_t vaddr) {
    return vaddr >= KERNEL_PAGE_OFFSET && vaddr < KERNEL_DIRECT_END;
}

bool vaddr_is_kmap(uint32_t vaddr) {
    return vaddr >= KERNEL_KMAP_START && vaddr < KERNEL_KMAP_END;
}

// 给一个物理页，返回一个当前内核可访问的虚拟地址，无论是高端还是低端的
void* kmap(uint32_t paddr) {
    // 如果物理地址位于低地址区，那么直接加上3GB偏移量后返回
    if (paddr_is_lowmem(paddr)) {
        return direct_map_ptr(paddr);
    }

    lock_acquire(&kmap_lock);
    // 如果位于高地址区，那么需要从高端的128MB（实际上是124MB可用虚拟地址）中，寻找空闲的一个页来给他做映射
    for (uint32_t idx = 0; idx < KMAP_SLOT_CNT; idx++) {
        if (kmap_slots[idx] != 0) {
            continue;
        }
        // 根据找到的空位计算虚拟地址，然后填充页表项，之后返回
        uint32_t vaddr = KERNEL_KMAP_START + idx * PG_SIZE;
        uint32_t* pte = pte_ptr(vaddr);
        if (*pte & PG_P_1) {
            continue;
        }
        *pte = paddr | PG_P_1 | PG_RW_W | PG_US_S;
        asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
        kmap_slots[idx] = paddr;
        lock_release(&kmap_lock);
        return (void*)vaddr;
    }
    lock_release(&kmap_lock);
    PANIC("kmap: no free highmem slots");
    return NULL;
}

// 该函数主要是用来卸载一个高端地址映射的
// 如果一个虚拟地址对应的物理内存在低端地址，那么它直接返回，不做其他处理
void kunmap(void* _vaddr) {
    uint32_t vaddr = (uint32_t)_vaddr;
    // 如果不是高端地址映射范围的地址，直接返回
    if (!vaddr_is_kmap(vaddr)) {
        return;
    }

    // 就是kmap的反向逻辑，情况一下页表项和slot啥的
    uint32_t idx = (vaddr - KERNEL_KMAP_START) / PG_SIZE;
    lock_acquire(&kmap_lock);
    ASSERT(idx < KMAP_SLOT_CNT);
    ASSERT(kmap_slots[idx] != 0);

    kmap_slots[idx] = 0;
    *pte_ptr(vaddr) = 0;
    asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
    lock_release(&kmap_lock);
}

void block_desc_init(struct mem_block_desc* desc_array){
	uint16_t desc_idx,block_size = 16;
	for(desc_idx=0;desc_idx<DESC_TYPE_CNT;desc_idx++){
		desc_array[desc_idx].block_size = block_size;
		// arena 元信息已经外置到 struct page 中，因此 payload 现在是一整页。
		desc_array[desc_idx].block_per_arena = PG_SIZE / block_size;

		dlist_init(&desc_array[desc_idx].free_list);
		block_size*=2;
	}
}

// return the addr of the [idx]th block in the arena
// 该函数的原理就是：
// 我们系统中的每一个物理页都对应一个 page 结构体，我们的 arena 元信息就存在这个 page 结构体中
// 通过 PAGE_TO_ADDR 我们获取 page 所对应的物理页的物理地址
// 然后我们将这个物理地址映射成虚拟地址，再加上偏移量返回
// PAGE_TO_ADDR 操作可以直接得到 page 结构体所映射到的那个物理页
static struct mem_block* arena2block(struct page* a,uint32_t idx){
    ASSERT(a->slab_desc != NULL);
    uint32_t arena_paddr = PAGE_TO_ADDR(&kernel_pool, a);
    // 我们的kmalloc通常申请的都是需要能稳定存在的对象
    // 因此一般都是从直接映射的低端内存取
    uint32_t arena_vaddr = (uint32_t)direct_map_ptr(arena_paddr);
	return (struct mem_block*)(arena_vaddr + idx * a->slab_desc->block_size);
}

// 由于我们现在将 arena 元信息放到 page 里面了
// 因此对于一个 mem_block 直接取页号，就可以得到对应的页
// 然后通过得到其物理地址，以这个物理地址为索引
// 到 global_pages 数组里就可以找到对应的 page 结构体
static struct page* block2arena(struct mem_block* b){
    uint32_t arena_vaddr = ((uint32_t)b & 0xfffff000);
    uint32_t arena_paddr = addr_v2p(arena_vaddr);
	return ADDR_TO_PAGE(global_pages, arena_paddr);
}

// 专门给内核模块使用 (如文件系统、驱动)
void* kmalloc(uint32_t size) {
    return do_alloc(size);
}

// the granularity of size is 1byte 
// do_malloc 和 do_free 现在专门给内核使用，用户态的malloc逻辑我们已经提取出去了
static void* do_alloc(uint32_t size){
	struct buddy_pool* mem_pool = &kernel_pool;
	struct mem_block_desc* descs = k_block_descs;
	// if mem allocated above the pool
	if(!(size>0&&size<mem_pool->pool_size)){
		return NULL;
	}
	
	struct page* a;
	struct mem_block* b;
	lock_acquire(&mem_pool->lock);

	// if size above 1024Bytes, allocate 1 page directly
	if(size>1024){
		uint32_t page_cnt = DIV_ROUND_UP(size,PG_SIZE);
		a = vmalloc_page(PF_KERNEL,page_cnt,false);

		if(a!=NULL){
			memset(a,0,page_cnt*PG_SIZE);
            a = ADDR_TO_PAGE(global_pages, addr_v2p((uint32_t)a));
			a->slab_desc = NULL;
			a->slab_cnt = page_cnt;
			a->slab_large = true;
			lock_release(&mem_pool->lock);
			return direct_map_ptr(PAGE_TO_ADDR(&kernel_pool, a)); 
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
				
				a = vmalloc_page(PF_KERNEL,1,false);
				
				if(a==NULL){
				lock_release(&mem_pool->lock);
				return NULL;
				}
				
				memset(a,0,PG_SIZE);
                a = ADDR_TO_PAGE(global_pages, addr_v2p((uint32_t)a));

				a->slab_desc = &descs[desc_idx];
				a->slab_large = false;
				a->slab_cnt = descs[desc_idx].block_per_arena;
				uint32_t block_idx;
				enum intr_status old_status = intr_disable();
				
				// append the mem_block to the free_list one by one
				for(block_idx=0;block_idx<descs[desc_idx].block_per_arena;block_idx++){
					b = arena2block(a,block_idx);
					ASSERT(!dlist_find(&a->slab_desc->free_list,&b->free_elem));
					dlist_push_back(&a->slab_desc->free_list,&b->free_elem);
				}
				intr_set_status(old_status);
			}
		
		// dlist_pop_front returns  dlist_elem *
		// mem_block has member dlist_elem
		struct dlist_elem *tmp = dlist_pop_front(&(descs[desc_idx].free_list));
		b = member_to_entry(struct mem_block,free_elem,tmp);
        
		memset(b,0,descs[desc_idx].block_size);

		a= block2arena(b);
		a->slab_cnt--;
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

    // 如果遇到一个计数已经为0的块，那么直接返回
    if(pg->ref_count==0) {
        printk("pfree: warning, pfree 0 ref_count page\n");
        return;
    }

    enum intr_status old = intr_disable();

    // 递减引用计数
    pg->ref_count--;

    // 如果减1后，页的引用计数为 1，且当前页的第一所有者是自己，那么就直接将第一所有者置为空
    // 为什么这么做的详细原因可以参考 write_protect 函数引用计数部分的注释
    if(pg->ref_count == 1 && pg->first_owner != NULL && pg->first_owner == get_running_task_struct()){
        pg->first_owner = NULL;
        // vaddr 没有必要置为 NULL，因为在共享的情况下 vaddr 是一样的
        // 在这种情况下直接给他移出活跃队列，防止在 pick_page_from_activate_list 中被扫描到
        if(dlist_is_linked(&pg->activate_tag)){
            dlist_remove(&pg->activate_tag);
        }
    }

    // 判断是否需要归还给伙伴系统
    if (pg->ref_count == 0) {
        // 这里的 pool 判断逻辑之前的一致
        struct buddy_pool* m_pool = (pg_phy_addr >= user_pool.phy_addr_start) ? &user_pool : &kernel_pool;
        pg->first_vaddr = 0;
        pg->first_owner = NULL;
        
        // 如果在活跃队列中，将其从活跃队列中移除
        if(dlist_is_linked(&pg->activate_tag)){
            dlist_remove(&pg->activate_tag);
        }

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
	if(pf==PF_KERNEL){
        intr_set_status(old);
        return;
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
        // 直接映射区不需要释放页表项
        if (vaddr_is_directmap(cur_vaddr)) { 
            pfree(cur_vaddr - KERNEL_PAGE_OFFSET);
            continue;
        }
        
        // 检查页表
        uint32_t* pte = pte_ptr(cur_vaddr);
        // 如果 P 位为 1，说明已经建立了物理映射，需要回收
        // 否则的话可能还是处于待分配的状态，没必要回收物理页
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

    if (pf == PF_KERNEL) { // 内核内存池处于直接映射区，不需要额外操作 
        return;
    }

    // 再回收虚拟部分（位图或 VMA）
    vaddr_remove(pf, _vaddr, pg_cnt);
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    do_free(ptr);
}

// 核心的内存释放引擎
// 用户通常的malloc和free操作是不会将堆的vma挖出洞来的，只是会回收物理内存而已
// 除非用户手动调用munmap等操作，不然的话即使释放到堆vma中间的部分，他也只是物理清空
// 虚拟不收缩，直到重新访问时触发页错误来分配
static void do_free(void* ptr){
    // put_str("ptr: ");put_int(ptr);put_str("\n");
	
    ASSERT(ptr!=NULL);
	if(ptr==NULL) return;

    struct buddy_pool* mem_pool = &kernel_pool;

	lock_acquire(&mem_pool->lock);
    
    struct mem_block* b = ptr;
    struct page* a = block2arena(b);
    

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

    ASSERT(a->slab_large==0||a->slab_large==1);

    if(a->slab_desc==NULL&&a->slab_large==true){
        uint32_t page_cnt = a->slab_cnt;
        void* arena_vaddr = direct_map_ptr(PAGE_TO_ADDR(&kernel_pool, a));
        a->slab_cnt = 0;
        a->slab_large = false;
        mfree_page(PF_KERNEL, arena_vaddr, page_cnt);
    }else{

        dlist_push_back(&a->slab_desc->free_list,&b->free_elem);
        // check if all mem_block in the arena is empty
        // if so, then release the whole arena 
        if(++(a->slab_cnt)==a->slab_desc->block_per_arena){
            uint32_t block_idx;
            // remove free_elem in the mem_block from the free_list one by one 
            for(block_idx=0;block_idx<a->slab_desc->block_per_arena;block_idx++){
                struct mem_block* b = arena2block(a,block_idx);
                ASSERT(dlist_find(&a->slab_desc->free_list,&b->free_elem));
                dlist_remove(&b->free_elem);
            }
            
            // 对于内核来说，我们仍然使用位图来管理空间，因此还是沿用原本的 mfree_page
            // 不要制造缺页
            a->slab_desc = NULL;
            a->slab_cnt = 0;
            a->slab_large = false;
            mfree_page(PF_KERNEL, direct_map_ptr(PAGE_TO_ADDR(&kernel_pool, a)),1);
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

// 用来测试kmalloc和kfree的稳定性
// 为了减少侵入性，我们只是使用ASSERT来对错误进行拦截，不打印额外信息
void sys_test(){
    printk("sys_test: kmalloc/kfree test start\n");

    void* small = kmalloc(32);
    ASSERT(small != NULL);
    ASSERT(((uint32_t)small & 0x7) == 0);
    memset(small, 0x5a, 32);
    struct page* small_pg = ADDR_TO_PAGE(global_pages, addr_v2p((uint32_t)small));
    ASSERT(small_pg->slab_desc != NULL);
    ASSERT(!small_pg->slab_large);
    ASSERT(small_pg->slab_cnt < small_pg->slab_desc->block_per_arena);

    void* medium = kmalloc(1000);
    ASSERT(medium != NULL);
    memset(medium, 0xa5, 1000);
    struct page* medium_pg = ADDR_TO_PAGE(global_pages, addr_v2p((uint32_t)medium));
    ASSERT(medium_pg->slab_desc != NULL);
    ASSERT(!medium_pg->slab_large);

    void* large = kmalloc(5000);
    ASSERT(large != NULL);
    memset(large, 0x3c, 5000);
    struct page* large_pg = ADDR_TO_PAGE(global_pages, addr_v2p((uint32_t)large));
    ASSERT(large_pg->slab_desc == NULL);
    ASSERT(large_pg->slab_large);
    ASSERT(large_pg->slab_cnt == 2);

    kfree(large);
    kfree(medium);
    kfree(small);

    printk("sys_test: kmalloc/kfree test done\n");
}

// 查找进程唯一的 brk heap VMA。
// 不能只靠 flags 匹配，因为匿名 mmap 区和 heap 可能具有完全相同的属性位。
// 对当前实现来说，真正的 heap 具有稳定特征：
// (1) 匿名、可读写、向上增长；
// (2) 起始地址固定等于进程的 end_data（即初始 start_brk）。
// 尤其是第二点，第一点的话，用mmap映射出的内存块也符合但他不一定是堆
static struct vm_area* find_heap_vma(struct task_struct* task) {
    struct dlist_elem* e = task->vma_list.head.next;
    while (e != &task->vma_list.tail) {
        struct vm_area* v = member_to_entry(struct vm_area, vma_tag, e);
        if (v->vma_start == task->end_data &&
            v->vma_inode == NULL &&
            v->vma_flags == (VM_READ | VM_WRITE | VM_GROWSUP | VM_ANON)) {
            return v;
        }
        e = e->next;
    }
    return NULL;
}

static bool is_kernel_vaddr(uint32_t vaddr) {
    return vaddr >= KERNEL_PAGE_OFFSET;
}

static uint32_t prot_to_vm_flags(uint32_t prot, bool anon) {
    uint32_t vma_flags = 0;
    if (prot & PROT_READ) {
        vma_flags |= VM_READ;
    }
    if (prot & PROT_WRITE) {
        vma_flags |= VM_WRITE;
    }
    if (prot & PROT_EXEC) {
        vma_flags |= VM_EXEC;
    }
    if (anon) {
        vma_flags |= VM_ANON;
    }
    return vma_flags;
}

// 给一个地址 vaddr 要么返回覆盖它的那个 VMA
// 要么返回起始地址在它后面的第一个 VMA
// 用于 munmap 这类区间操作，既能处理命中的 VMA，也能跳过中间的空洞。
// 我们不能用原来的那个 find_vma，因为那个函数只会返回 vaddr 所在的 vma
// 要是 vaddr 在空洞里面的话就什么都不会返回了
// munmap(addr, len) 处理的是一个区间，这个区间里可能是：
// [VMA][空洞][VMA][VMA]
// 如果只是用 find_vma, 一旦 cursor 落到空洞，就拿不到后面的 VMA 了
// 那就没法继续处理这个区间后半段
// 这也是我们为什么要拿起始地址在 vaddr 后面的第一个 VMA
// 因为这个 VMA 很可能会在 vaddr + size 区间所覆盖的范围内，此时需要释放它或者切分它
static struct vm_area* find_covering_or_next_vma(struct task_struct* task, uint32_t vaddr) {
    struct dlist_elem* e = task->vma_list.head.next;
    while (e != &task->vma_list.tail) {
        struct vm_area* v = member_to_entry(struct vm_area, vma_tag, e);
        // 返回的这个 VMA 要么就直接命中了，要么就是起始地址在这个 vaddr 之后的 VMA
        if (vaddr < v->vma_end) {
            return v;
        }
        e = e->next;
    }
    return NULL;
}

// 手动填写低端内存映射
static void direct_map_lowmem_range(uint32_t start_paddr, uint32_t end_paddr) {
    uint32_t paddr = PAGE_ALIGN_DOWN(start_paddr);
    uint32_t limit = PAGE_ALIGN_UP(end_paddr);
    uint32_t page_flags = PG_P_1 | PG_RW_W | PG_US_S;

    while (paddr < limit) {
        uint32_t vaddr = KERNEL_PAGE_OFFSET + paddr;
        uint32_t* pde = pde_ptr(vaddr);
        uint32_t* pte = pte_ptr(vaddr);
        
        if (!(*pde & PG_P_1)) {
            // 按理说我们已经在loader里面预留了这些页目录项和页表空间，不会报这个错
            PANIC("direct_map_lowmem_range: missing PDE, please extend loader page tables");
        }
        if (!(*pte & PG_P_1)) {
            *pte = paddr | page_flags;
            asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
        } else if ((*pte & 0xfffff000) != paddr) {
            PANIC("direct_map_lowmem_range: conflicting direct map entry");
        }
        paddr += PG_SIZE;
    }
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
    // brk只能扩用户栈，不能碰内核区域
    if (new_brk >= USER_STACK_BASE) return cur->brk; 

    struct vm_area* heap_vma = find_heap_vma(cur);
    ASSERT(heap_vma != NULL);

    uint32_t old_brk_aligned = PAGE_ALIGN_UP(cur->brk);
    uint32_t new_brk_aligned = PAGE_ALIGN_UP(new_brk);

    // 超过用户空间上界，或 PAGE_ALIGN_UP 发生回绕，都视为非法请求。
    // new_brk_aligned < new_brk 说明上对齐后的空间比用户请求的还低，那么说明发生了溢出回绕
    if (new_brk_aligned < new_brk || new_brk_aligned >= USER_STACK_BASE) {
        return cur->brk;
    }

    //  缩小堆
    if (new_brk < cur->brk) {
        // 只有当对齐后的边界发生回退，才需要真正“收地”
        if (new_brk_aligned < old_brk_aligned) {
            // 堆收缩时只回收物理页和页表映射，不销毁 heap VMA 本身。
            // heap VMA 需要长期保留，由 sys_brk 负责维护其页级边界；
            // 即使堆缩回起点，也应该表现为一个空的 [start_brk, start_brk) 区间。
            uint32_t release_pg_cnt = (old_brk_aligned - new_brk_aligned) / PG_SIZE;
            
            // 调用 mfree_physical_pages 而不是 mfree_pages
            // 因为 mfree_pages 会把虚拟地址一并给释放了！从而导致堆的 VMA 没了
            mfree_physical_pages((void*)new_brk_aligned, release_pg_cnt);
            
            // 更新 VMA 边界
            // 我们的堆VMA与物理页的释放保持一致
            // 只有发生页级别的回收时我们再更改 VMA 
            // 这么做最主要是为了简单，这么做页级懒分配也比较简单
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

    // 更新 VMA 边界（画饼，不实际分配内容，直到发生页错误，让swap_page来分配）
    heap_vma->vma_end = new_brk_aligned;
    cur->brk = new_brk;

    return cur->brk;
}

// 该函数最核心的功能就是找空隙，然后建立vma
// 具体的内存分配操作交给缺页中断来做
static uint32_t do_mmap(struct task_struct* cur, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t offset) {
    if (len == 0) {
        return (uint32_t)MAP_FAILED;
    }
    if (addr != 0) {
        return (uint32_t)MAP_FAILED;
    }
    // 目前仅支持 MAP_PRIVATE，匿名映射和文件私有映射都走这一条。
    if ((flags & MAP_PRIVATE) == 0) {
        return (uint32_t)MAP_FAILED;
    }
    if (flags & ~(MAP_PRIVATE | MAP_ANON)) {
        return (uint32_t)MAP_FAILED;
    }
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
        return (uint32_t)MAP_FAILED;
    }

    uint32_t size = PAGE_ALIGN_UP(len); // 我们按页为单位来进行映射
    if (size == 0) {
        return (uint32_t)MAP_FAILED;
    }
    uint32_t pg_cnt = size / PG_SIZE;
    // 从高到底找空隙插入vma，防止和heap打架
    uint32_t vaddr = vma_find_gap_reverse(cur, pg_cnt);
    if (vaddr == 0) {
        return (uint32_t)MAP_FAILED;
    }

    // vaddr + size <= vaddr 是在校验溢出
    // vaddr + size > (USER_STACK_BASE - USER_STACK_SIZE) 防止装上用户栈
    // 我们默认预留了8MB的用户栈
    if (vaddr < USER_VADDR_START || vaddr + size <= vaddr || vaddr + size > (USER_STACK_BASE - USER_STACK_SIZE)) {
        return (uint32_t)MAP_FAILED;
    }

    // 如果是匿名映射，只分配vma，然后等待缺页惰性分配
    if (flags & MAP_ANON) {
        if (fd != -1 || offset != 0) {
            return (uint32_t)MAP_FAILED;
        }
        add_vma(cur, vaddr, vaddr + size, 0, NULL, prot_to_vm_flags(prot, true), 0);
        return vaddr;
    }

    // 只支持映射起始文件偏移为页对齐的 mmap 请求。
    // 按理说非页对齐也能做，但是太复杂了我们暂时先不做
    if ((offset & (PG_SIZE - 1)) != 0) {
        return (uint32_t)MAP_FAILED;
    }
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC || cur->fd_table[fd].global_fd_idx == -1) {
        return (uint32_t)MAP_FAILED;
    }

    uint32_t global_fd = fd_local2global(cur, (uint32_t)fd);
    struct file* mmap_file = &file_table[global_fd];
    if (file_mmap(mmap_file, vaddr, size, prot, flags, offset) < 0) {
        return (uint32_t)MAP_FAILED;
    }
    return vaddr;
}

uint32_t sys_mmap(uint32_t user_mmap_args) {
    struct task_struct* cur = get_running_task_struct();
    struct mmap_args* user_args = (struct mmap_args*)user_mmap_args;

    // sys_mmap 只能被用户进程调用
    if (cur->pgdir == NULL) {
        return (uint32_t)MAP_FAILED;
    }
    if (user_args == NULL) {
        return (uint32_t)MAP_FAILED;
    }
    // 校验传入的参数包是否是在用户空间中
    if (user_mmap_args < USER_VADDR_START ||
        user_mmap_args + sizeof(struct mmap_args) <= user_mmap_args || // 校验溢出
        user_mmap_args + sizeof(struct mmap_args) > USER_STACK_BASE) {
        return (uint32_t)MAP_FAILED;
    }

    return do_mmap(cur,
                          user_args->addr,
                          user_args->len,
                          user_args->prot,
                          user_args->flags,
                          user_args->fd,
                          user_args->offset);
}

// 用于适配 mmap2
// 主要区别就是 mmap 是用参数包来进行参数传递的，而 mmap2 直接用寄存器传参，速度更快
// 此外，在 32 位系统上，寄存器是 32 位的。原始的 mmap 最后一个参数 offset 是以字节为单位的。
// 这意味着 mmap 无法映射文件中超过 4GB 位置之后的数据。
// 为了支持大文件，Linux 引入了 mmap2。它规定最后一个参数不再是字节，而是页面索引，从而进行大范围的索引跨越
// Musl 几乎总是优先调用 mmap2
uint32_t sys_mmap_direct(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t offset) {
    struct task_struct* cur = get_running_task_struct();
    if (cur->pgdir == NULL) {
        return (uint32_t)MAP_FAILED;
    }
    return do_mmap(cur, addr, len, prot, flags, fd, offset);
}

int32_t sys_munmap(uint32_t addr, uint32_t len) {
    struct task_struct* cur = get_running_task_struct();
    if (cur->pgdir == NULL) {
        return -1;
    }
    if (len == 0 || (addr & (PG_SIZE - 1)) != 0) {
        return -1;
    }

    uint32_t size = PAGE_ALIGN_UP(len);
    if (size == 0) {
        return -1;
    }
    uint32_t end = addr + size;
    if (addr < USER_VADDR_START || end <= addr || end > USER_STACK_BASE) {
        return -1;
    }

    // 第一遍只做校验，避免解除映射进行到一半才发现范围里混入了 heap/stack。
    uint32_t cursor = addr;
    while (cursor < end) {
        // 获取当前vaddr所在的vma或者其实地址在这个vaddr之后的第一个vma
        struct vm_area* vma = find_covering_or_next_vma(cur, cursor);
        // 假如获取到的第一个vma的vma_start以及在我们想要释放的区间之外了
        // 那么就不用释放了
        if (vma == NULL || vma->vma_start >= end) {
            break; // 剩余区间里已经没有映射了，等价于 no-op
        }

        // 如果 vaddr 在空洞的话，跳过空洞
        if (cursor < vma->vma_start) {
            cursor = vma->vma_start; // 跳过空洞
        }

        // VM_GROWSUP 和 VM_GROWSDOWN 就是 heap/stack，这个区间是不能被 munmap 释放的
        if ((vma->vma_flags & (VM_GROWSUP | VM_GROWSDOWN)) != 0) {
            return -1;
        }

        // 移动游标，检查下一个 vma
        uint32_t seg_end = end < vma->vma_end ? end : vma->vma_end;
        cursor = seg_end;
    }

    // 第二遍真正执行解除映射，支持一个区间跨多个普通 mmap VMA。
    cursor = addr;
    while (cursor < end) {
        struct vm_area* vma = find_covering_or_next_vma(cur, cursor);
        if (vma == NULL || vma->vma_start >= end) {
            break;
        }

        // 跳过空洞
        if (cursor < vma->vma_start) {
            cursor = vma->vma_start;
        }

        uint32_t seg_end = end < vma->vma_end ? end : vma->vma_end;
        // 由于除以了一个PG_SIZE，因此如果一个区间段小于一个页的话可能不会被释放到
        // 因此我们才强制在 mmap 里面要求以页为单位进行映射，才调用了一个 PAGE_ALIGN_UP
        // 这是一个局限，需要注意
        mfree_page(PF_USER, (void*)cursor, (seg_end - cursor) / PG_SIZE);
        cursor = seg_end;
    }
    return 0;
}

// owner_pgdir: 目标进程页目录的虚拟地址（通常存在 task_struct 里）
// vaddr: 要查找的虚拟地址
// 返回值：指向目标 PTE 的内核虚拟地址指针
uint32_t* get_pte_ptr(uint32_t* pgdir, uint32_t vaddr) {

    ASSERT(pgdir != NULL);

    // 获取 PDE 索引和 PTE 索引
    uint32_t pde_idx = (vaddr & 0xffc00000) >> 22;
    uint32_t pte_idx = (vaddr & 0x003ff000) >> 12;

    // 找到 PDE
    uint32_t pde = pgdir[pde_idx];

    // 检查 PDE 是否存在
    // 如果页表（Page Table）本身还没分配，说明这个 vaddr 从未被映射过
    if (!(pde & PG_P_1)) {
        return NULL; 
    }

    // 从 PDE 中提取页表的物理地址（高 20 位）
    uint32_t pt_phyaddr = pde & 0xfffff000;

    // 将页表物理地址转换为内核可访问的虚拟地址
    uint32_t* pt_vaddr = (uint32_t*)direct_map_ptr(pt_phyaddr);

    // 返回指向具体 PTE 的指针
    return &pt_vaddr[pte_idx];
}