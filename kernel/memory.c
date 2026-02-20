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
#include "buddy.h"


#define PDE_IDX(addr) ((addr&0xffc00000)>>22)
#define PTE_IDX(addr) ((addr&0x003ff000)>>12)
// find which start vaddr will map to this pte and pde
#define PTE_TO_VADDR(pde_idx,pte_idx) (((pde_idx)<<22)|((pte_idx)<<12))
#define PAGE_TABLE_VADDR(pde_idx) (0xffc00000|(pde_idx<<12))

#define K_TEMP_PAGE_VADDR 0xff3ff000  

#define BOOTSTRAP_VMA_COUNT 32

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

static void* palloc(struct buddy_pool* m_pool);
int32_t inode_read_data(struct m_inode* inode, uint32_t offset, void* buf, uint32_t count);
static struct vm_area* find_vma_condition(struct task_struct* task, uint32_t flags);
static struct vm_area* vma_alloc_bootstrap(void);

// 内核虚拟地址空间的 VMA 链表
struct dlist kernel_vma_list;

// 保护内核 VMA 链表的信号量
struct lock kernel_vma_lock;

// 自举期使用的 vma 内存池
// 由于 kmalloc 依赖虚拟地址分配函数，而虚拟地址分配又依赖于vma
// 并且vma的添加等操作又依赖于 kmalloc
// 因此这就形成一个死循环了，为了打破这个死循环，我们在早期初始化时使用静态数组来作为内存池
// 从这里取出vma来进行初始化，待到vma树建立完毕后再使用kmalloc来创建
// 这里的这一部分vma由于和内核代码高度相关，因此基本都是只借不还的
static struct vm_area vma_bootstrap_pool[BOOTSTRAP_VMA_COUNT];

static uint32_t vma_idx = 0;

static struct vm_area* vma_alloc_bootstrap() {
    if (vma_idx < BOOTSTRAP_VMA_COUNT) {
        // 拿到当前可用的结构体，然后索引自增
        struct vm_area* vma = &vma_bootstrap_pool[vma_idx++];
        // 顺手清理一下，确保它是干净的
        memset(vma, 0, sizeof(struct vm_area));
        return vma;
    }
    // 如果 64 个全用完了，说明初始化逻辑映射了太多奇怪的东西
    PANIC("VMA bootstrap pool exhausted!");
    return NULL;
}

static void early_vma_init(void) {
    // 初始化全局链表和锁
    dlist_init(&kernel_vma_list);
    lock_init(&kernel_vma_lock);

    // 从静态池拿种子VMA
    struct vm_area* reserved_vma = vma_alloc_bootstrap(); // 内核保留区
    struct vm_area* heap_vma = vma_alloc_bootstrap();
    
    kernel_heap_start = PAGE_ALIGN_UP(kernel_heap_start);

    reserved_vma->vma_start = KERNEL_VADDR_START;
    reserved_vma->vma_end   = kernel_heap_start;
    reserved_vma->vma_flags = VM_READ|VM_WRITE;
    reserved_vma->vma_inode = NULL;
    
    // 填充,预留32MB内核堆空间
    // 由于 add_vma 函数底层调用了 kmalloc，但是此时我们的堆还没准备好呢
    // 因此不能用 add_vma 来添加内核堆，我们得手动添加
    
    heap_vma->vma_start = kernel_heap_start;
    heap_vma->vma_end   = PAGE_ALIGN_UP(kernel_heap_start + (32 * 1024 * 1024));
    heap_vma->vma_flags = VM_READ | VM_WRITE | VM_GROWSUP | VM_ANON;
    heap_vma->vma_inode = NULL;

    dlist_push_back(&kernel_vma_list, &reserved_vma->vma_tag);

    // 挂载到全局链表
    // 因为是初始化，此时没竞争，可以不拿锁
    dlist_push_back(&kernel_vma_list, &heap_vma->vma_tag);

    // 由于我们的内核栈是位于用户PCB中的
    // 而用户PCB这块区域肯定是被事先分配好的，所以不可能会触发页错误
    // 因此即使不使用VMA来管理内核栈也不会出问题

    put_str("Kernel global VMA list initialized with static heap.\n");
}

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
        vaddr_start = vma_find_gap(pf, pg_cnt);
        
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
static void* palloc(struct buddy_pool* m_pool) {
    // 关中断保证原子性（因为涉及 pool 中空闲链表的修改）
    enum intr_status old = intr_disable();

    // 调用伙伴系统的核心分配函数，申请 1 个物理页 (order 0)
    // 伙伴系统内部会处理 lock
    struct page* pg = palloc_pages(m_pool, 0);

	// put_str("DEBUG: pg struct addr: "); put_int((uint32_t)pg); 
    // put_str(", pool used: "); put_str(m_pool == &kernel_pool ? "KERNEL" : "USER");
    // put_str("\n");

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
    // 通过该接口申请的页默认使用大内存分配
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
        // 用户态，既然 vaddr_alloc 已经 add_vma 了，我们直接返回！
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
            debug_printk("DEBUG: Pointer %x has no physical page. (Page already recycled)\n", vaddr);
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
            // 物理回收，节省物理内存
            // 虚拟内存保留，维持堆的连续性，不调用 vaddr_remove
            // 后续如果再被分配到，可以通过缺页中断来重新进行物理页的分配
            mfree_physical_pages(a,1);
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
                struct page* pg = ADDR_TO_PAGE(global_pages,pa);
    			pg->ref_count++;

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


static bool is_vaddr_mapped(struct task_struct* task, uint32_t vaddr) {
    // 先检查页目录项 
    uint32_t* pde = pde_ptr(vaddr);
    if (!(*pde & PG_P_1)) {
        // 如果 PDE 都不存在，说明对应的页表页还没分配，物理页肯定没映射
        return false; 
    }

    // 只有 PDE 存在，访问 pte_ptr 才不会导致崩溃
    uint32_t* pte = pte_ptr(vaddr);
    return (*pte & PG_P_1);
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

    // 防止swap_page递归重入，掩盖错误的第一现场
    // 如果报错地址小于 0x1000（第一页），通常不是懒加载，而是代码 Bug，
    // 直接杀掉，不要尝试查找 VMA：
    if (vaddr < 0x1000) {
        printk("CRITICAL: Null Pointer Access at %x! Terminating.\n", vaddr);
        while(1);
    }

	// 先尝试在 VMA 合同库里找找看，这个地址合法吗？
    struct vm_area* vma = find_vma(cur, page_vaddr);

#ifdef DEBUG_VMA
    printk("page_vaddr:%x vma:%x\n",page_vaddr,vma);
#endif
    // 如果 vma 都不存在，那么说明不是懒加载，而是真正的段错误 
	if (vma == NULL) {
        printk("VMA Search Failed! vaddr: %x\n", page_vaddr);
        // vma = find_vma_condition(cur, VM_READ|VM_WRITE|VM_GROWSDOWN|VM_ANON);
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
    // while(1);
	// 合法合同且尚未映射，开始分配物理页
    // mapping_v2p 内部会完成 palloc 物理页 + 修改位图 + 建立页表映射
    void* ret_vaddr =  mapping_v2p(PF_USER, page_vaddr);
    if (ret_vaddr == NULL) {
        // 物理内存耗尽，且没有置换算法
        PANIC("Out of Memory! Cannot allocate physical page for vaddr\n");
    }

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

    debug_printk("write_protect:::err_code: %d, err_vaddr: %x\n", err_code, err_vaddr);

    // 内核触发写保护：直接 PANIC，因为内核不参与 COW
    if (cur->pgdir == NULL || (uint32_t)err_vaddr >= 0xC0000000) {
        PANIC("Kernel Write Protection Error!");
    }

	uint32_t vaddr = (uint32_t)err_vaddr;

    struct vm_area* vma = find_vma(cur, vaddr);
    if (vma == NULL){
        PANIC("write_protect: vma == NULL");
    }
    
	// 先判断当前发送只读页错误的地址是不是位于数据段
	// 要是位于代码段的话那么本来也就不让改写
	// 直接发信号终止程序

	// 来判断是否为代码段
    if (!(vma->vma_flags & VM_WRITE)) {
        printk("PID %d (%s) attempt to write Read-Only Segment at %x\n", cur->pid, cur->name, vaddr);
        send_signal(cur, SIGSEGV);
		intr_set_status(_old);
        return;
    }

	uint32_t* pte = pte_ptr(vaddr);
    uint32_t pa = *pte & 0xfffff000;

    // COW 处理
	struct page* pg = ADDR_TO_PAGE(global_pages,pa);

    if (pg->ref_count > 1) {
        // 确实有多个进程共享，执行拷贝
        do_copy_on_write(vaddr, pte, pa);
    } else if (pg->ref_count == 1) {
        // 只有一个人用了，直接恢复写权限，不用额外拷贝了
        *pte |= PG_RW_W;
		// 刷新页目录项
        asm volatile ("invlpg %0" : : "m" (*(char*)vaddr));
    } else {
        PANIC("write_protect: global_pages[] counter error!");
    }

	intr_set_status(_old);
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