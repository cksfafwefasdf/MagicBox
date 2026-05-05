#include <swap.h>
#include <vma.h>
#include <memory.h>
#include <global.h>
#include <buddy.h>
#include <interrupt.h>
#include <debug.h>
#include <inode.h>
#include <stdio-kernel.h>
#include <process.h>
#include <bitmap.h>
#include <stdint.h>
#include <wait_exit.h>
#include <ide.h>
#include <stdbool.h>
#include <sync.h>
#include <errno.h>

// 为了快速索引，用数组存指针
// 设备号从 1 开始，以便避免创建出的 pte 最终为 0 的情况
struct swap_info* swap_table[MAX_SWAP_DEVICES+1];

struct lock swap_lock;

static void* swap_out(void);
static void swap_write(uint32_t pte_val, void* buf);
static void swap_read(uint32_t pte_val, void* buf);
static bool swap_in(uint32_t* pte_ptr, uint32_t page_vaddr, struct vm_area* vma);

void swap_init(){
    memset(swap_table, 0, sizeof(swap_table));
    lock_init(&swap_lock);
}

// 将父进程的页表项设置为只读后拷贝给子进程
// 使得子进程和父进程在一开始共享完全一致的物理空间
// 发生写页错误后再开始分家
void copy_page_tables(struct task_struct* from, struct task_struct* to, void* page_buf) {
    ASSERT(page_buf != NULL);
	enum intr_status old_status = intr_disable();
    uint32_t* to_pte_buf = (uint32_t*)page_buf;

    // 遍历用户空间 PDE
	uint32_t pde_idx = 0;
    for (pde_idx = 0; pde_idx < USER_PDE_NR; pde_idx++) {
        uint32_t* from_pde = from->pgdir + pde_idx;
        if (!(*from_pde & PG_P_1)) continue;

        // 为子进程分配页表物理页
        uint32_t to_pt_pa = (uint32_t)palloc(&kernel_pool);
        if (!to_pt_pa) PANIC("copy_page_tables: palloc failed");

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

        void* to_pt_kaddr = kmap(to_pt_pa);
        memcpy(to_pt_kaddr, to_pte_buf, PG_SIZE);
        kunmap(to_pt_kaddr);

        // 将该页表挂载到子进程的页目录中
        to->pgdir[pde_idx] = to_pt_pa | PG_US_U | PG_RW_W | PG_P_1;
    }

    // 因为修改了大量父进程的 PTE 属性（RW -> RO），必须重载 CR3 彻底刷新
    page_dir_activate(from);
	intr_set_status(old_status);
}

// -d int -D qemu.log
// 该函数对应两者情况
// 懒加载/交换：内核分配物理页。
// 非法访问：访问了完全没有映射、或者不属于用户空间（如访问内核空间地址）的内存
void swap_page(uint32_t err_code,void* err_vaddr){

#ifdef DEBUG_SWAP_INFO
    // Error Code 在栈上，EIP 紧随其后（高地址方向）
    uint32_t* p_stack = &err_code;
    uint32_t fault_eip = p_stack[1]; // eip 就在 error_code 的上方
    uint32_t fault_esp = p_stack[4]; // esp 在更上方 (如果是从用户态进来的)

    printk("\n--- PAGE FAULT DEBUG ---\n");
    printk("Faulting VADDR: 0x%x\n", err_vaddr);
    printk("Faulting EIP:   0x%x\n", fault_eip);
    printk("Error Code:     0x%x (%s)\n", err_code, 
           (err_code & 1) ? "Page Present" : "Page Not Present");
#endif
	enum intr_status _old =  intr_disable();
#ifdef DEBUG_PG_FAULT
	printk("swap_page:::err_code: %d, err_vaddr: %x\n", err_code, err_vaddr);
#endif
	struct task_struct* cur = get_running_task_struct();
    ASSERT(cur!=NULL);
	uint32_t vaddr = (uint32_t)err_vaddr;

	// 硬件给出的 vaddr 可能是 0xbffffabc，我们需要把它对齐到 0xbffff000
    uint32_t page_vaddr = vaddr & 0xfffff000;

    // 防止swap_page递归重入，掩盖错误的第一现场
    // 如果报错地址小于 0x100000（低端1MB），通常不是懒加载，而是代码 Bug，
    // 主要是针对内核线程的限制，用户进程会发信号自己退出
    if (vaddr < 0x100000 && cur->pgdir == NULL) {
        printk("CRITICAL: Null Pointer Access at %x! Terminating.\n", vaddr);
        print_stacktrace();
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

    // 获取 PTE 状态
    uint32_t* pte_ptr = get_pte_ptr(cur->pgdir, page_vaddr);
    uint32_t pte_val = (pte_ptr) ? *pte_ptr : 0;

    // 页面在交换分区中 (P=0 且 PTE 有内容)
    if (pte_val != 0 && !(pte_val & PG_P_1)) {
        if (!swap_in(pte_ptr, page_vaddr, vma)) {
            printk("swap_page: swap_in failed");
            intr_set_status(_old);
            goto segmentation_fault;
        }
        intr_set_status(_old);
        return;
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
    
    // pte_val!=0 但是 (pte_val & PG_P_1) == 1 时的情况
    if(pte_val!=0 && (pte_val & PG_P_1)){
        // 如果 P=1 依然进到这里，说明不是缺页，可能是写保护，但这种情况需要交由 write_protect 处理
        PANIC("swap_page: unexpected fault");
    }

    // while(1);
	// 合法合同且尚未映射，开始分配物理页
    // mapping_v2p 内部会完成建立页表映射以及初始化一些基本状态，物理内存需要我们手动申请
    void* page_paddr = NULL;

    // 进行尽力而为的内存分配，一直申请，直到成功或者内存耗尽
    // 这么做在多核的情况下也比较好
    while (1) {
        page_paddr = palloc(&user_pool);
        if (page_paddr != NULL) {
            break; // 申请成功，跳出循环
        }

        // 走到这里说明 palloc 失败了
        // 内存满了，尝试踢出一个页
        void* swapped_phys = swap_out();

        if (swapped_phys == NULL) {
            // 连 swap_out 都踢不出页面了（比如内存里全是内核不可移动页或被锁定的页）
            // 此时才是真正的 Out of Memory，而不是内存负载过大
            printk("swap_page: out of memory! No page can be swapped out.\n");
            goto segmentation_fault;
        }

        // 既然 swap_out 成功返回了一个物理地址，说明理论上现在有一页空闲了
        // 循环会回到开头，再次执行 palloc
        // 我们最好就用系统原本的 palloc 和 pfree 函数，这样接口比较统一
        // 在单核的情况下，我们通过关中断，理论上来将，下一次 palloc 应该都能成功
        // 但是在多核的情况下，刚刚 swap_out 出的内存可能会被其他的核拿走，只是关中断没用
        // 因此我们需要使用 while 来进行多次尝试
    }

    // 成功拿到 page_paddr，进行映射
    mapping_v2p(page_vaddr, (uint32_t)page_paddr);

	// 根据合同内容初始化物理页数据
    // 内核最好不要用用户的虚拟地址，最好临时映射一个用
    void* kaddr = kmap((uint32_t)page_paddr);
    if (vma->vma_inode != NULL) {
        // 有文件的映射 (代码段、数据段、BSS)
        uint32_t offset_in_vma = page_vaddr - vma->vma_start;
        
        // 先全页清零，保证了 BSS 区域和文件末端对齐部分的正确性
        memset((void*)kaddr, 0, PG_SIZE);

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
                            (void*)kaddr, 
                            read_size);
        }
    } else {
        // 匿名映射 (栈、堆 brk 区域) 
        // 按照规定，新分配的匿名页必须初始化为全 0
		// 这里可以直接自动实现我们的栈扩容逻辑
        memset((void*)kaddr, 0, PG_SIZE);
    }
    kunmap(kaddr);

	intr_set_status(_old);
    return;

segmentation_fault:
        if (cur->pgdir != NULL) {
            printk("PID %d (%s) Segmentation Fault at %x\n", cur->pid, cur->name, vaddr);
        
        // 检查用户是否自定义了 SIGSEGV 的处理函数
        struct sigaction* sa = &cur->sigactions[SIGSEGV - 1];
        
        if (sa->sa_handler == SIG_DFL) {
            // 用户没管这个信号。直接杀掉，这是最稳妥的，防止产生死循环。
            sys_exit(-SIGSEGV); 
        } else {
            // 用户想自己处理。
            // 这时发信号，并允许 iret 回去。
            // 但是如果用户处理函数里又访问了 0 地址，还是会死循环，
            // 这时需要靠 sa_mask 屏蔽位或者内核的递归深度检查来防御。
            send_signal(cur, SIGSEGV);
        }
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

    // 在原本的实现中，我们是通过一个固定的K_TEMP_PAGE_VADDR来进行数据转运的
    // 现在我们是通过动态映射的方式来进行转运
    void* new_page_kaddr = kmap((uint32_t)new_pa);

    // 执行物理内存数据的搬运
    // 源地址：故障发生的虚拟页起始地址 (vaddr & 0xfffff000)
    // 我们将发生写保护错误的那个虚拟地址所对应的数据全部拷贝到我们新映射出的物理页中
    memcpy(new_page_kaddr, (void*)(vaddr & 0xfffff000), PG_SIZE);
    // 拷贝完毕后，把临时映射的虚拟地址给释放了
    kunmap(new_page_kaddr);

    // 更新原虚拟地址的映射关系，直接更新页表就行
    // 我们更新的是当前进程的页表，也就是说，谁进行的写操作，谁进行复制
    // 并且复制完后自己更新自己的页表，和当前进程共享页面的其他进程的页表不变
    // 谁写的谁自己主动搬出去
    // 如果只有一个引用计数的话，我们会在 write_protect 里面直接恢复写权限，不会走到这个函数里面
    // 现在 PTE 指向新物理页，并开启 PG_RW_W 写权限
    *pte = (uint32_t)new_pa | PG_P_1 | PG_RW_W | PG_US_U;

    // 谁写的谁自己搬出去，然后自己重新创建一个 swap 信息
    struct page* pg = ADDR_TO_PAGE(global_pages,new_pa);
    pg->first_owner = get_running_task_struct();
    pg->first_vaddr = vaddr & 0xfffff000;

    // 调用pfree减去老物理页的引用计数
    // 变成 0 时会自动释放，但是在此处应该不会变成 0
    pfree(old_pa);

    // 刷新出错虚拟地址的 TLB
    // 使 CPU 意识到该地址现在已经是新物理页且可写了
    asm volatile ("invlpg %0" : : "m" (*(char*)vaddr) : "memory");
}

// 写时复制（COW）：这是合法的，内核分配新页并映射，然后直接 ret 返回用户态继续执行。
// 非法写入：比如用户程序试图修改只读的代码段（.text）。
void write_protect(uint32_t err_code, void* err_vaddr) {
	enum intr_status _old =  intr_disable();
	
    struct task_struct* cur = get_running_task_struct();
#ifdef DEBUG_PG_FAULT
    printk("write_protect:::err_code: %d, err_vaddr: %x\n", err_code, err_vaddr);
#endif

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

    // 我们在这里处理了 swap 的两种特殊情况，一种是一个页触发写保护后，拷贝了这个页
    // 这时我们需要将拷贝出来的这个页的 swap 所有页设置为这个写进程
    // 对应 do_copy_on_write 中的处理逻辑
    // 第二种情况是 A 和 B 共享一个页，A 是第一所有者，此时 A 退出了，因此我们需要将第一所有者改成唯一存活的这个 B
    // 但是我们这样的处理不太彻底，因为假如 B 自始至终都没有触发写保护错误的话，这个页的所有者自始至终都不会被修正
    // 如果在此期间我们要针对这个页进行 swap 的话，页表查询的操作就会出错，因为原本的 A 进程都已经不在了，查它的页表会段错误
    // 对于这个问题的解决办法是在 pfree 中，当引用计数为 2 且 owner 是自己时，直接把 owner 置为空并将其从活跃队列中移出
    // 等到后续触发写保护的时候再来重新更新它的所有者，并将其加入活跃队列，这么做会导致有一些页无法被置换出去，但是总比让系统直接崩溃了要好
    // 在我们目前的系统中，只有只读页的引用计数能够大于 1，可写的页的引用计数一定是为 1 的，因此这个方案目前来看至少不会让系统出错
    if (pg->ref_count > 1) {
        // 确实有多个进程共享，执行拷贝
        do_copy_on_write(vaddr, pte, pa);
    } else if (pg->ref_count == 1) {
        // 只有一个人用了，直接恢复写权限，不用额外拷贝了
        *pte |= PG_RW_W;
        // 更新一下所有者，防止 swap 的时候出现问题
        pg->first_owner = get_running_task_struct(); 
        pg->first_vaddr = vaddr & 0xfffff000;

        if(!dlist_is_linked(&pg->activate_tag)){
            dlist_push_back(&user_pool.activate_list, &pg->activate_tag);
        }

		// 刷新页目录项
        asm volatile ("invlpg %0" : : "m" (*(char*)vaddr));
    } else {
        PANIC("write_protect: global_pages[] counter error!");
    }

    // printk("COW Done: vaddr %x now mapped to pa %x\n", vaddr, *pte & 0xfffff000);

	intr_set_status(_old);
}

static int32_t get_swap_info_by_part(struct partition* part) {
    for (int i = 1; i <= MAX_SWAP_DEVICES; i++) {
        if (swap_table[i] && swap_table[i]->part == part) {
            return i;
        }
    }
    return -1;
}

static int32_t alloc_swap_dev_slot(void){
    for (int i = 1; i <= MAX_SWAP_DEVICES; i++) {
        if(swap_table[i]==NULL){
            return i;
        }
    }
    return -1;
}

void do_swapon(struct partition* part) {
    lock_acquire(&swap_lock);

    if (get_swap_info_by_part(part) > 0) {
        printk("do_swapon: Device %s already mounted as a swap device.\n", part->name);
        lock_release(&swap_lock);
        return;
    }

    int32_t dev_id = alloc_swap_dev_slot();
    if(dev_id<0){
        printk("do_swapon: fail to swapon! no more free swap dev slot\n");
        lock_release(&swap_lock);
        return;
    }

    struct swap_info* si = (struct swap_info*)kmalloc(sizeof(struct swap_info));

    if(si==NULL){
        PANIC("do_swapon: fail to kmalloc for swap_info");
    }
    
    si->part = part;
    // 我们减去一个 1，留出8个扇区的间隙，防止使用多分区时两个分区间隔的太近
    // 一个分区把另一个分区的分区表给写坏了，这种情况实在是难以避免，因此我们就采用最简单的留出间隙的方法
    si->slot_cnt = part->sec_cnt / 8 - 1; // 一个页面 8 个扇区
    
    // 初始化位图结构
    // 计算位图字节长度：slot_cnt / 8 向上取整
    si->slot_bitmap.btmp_bytes_len = DIV_ROUND_UP(si->slot_cnt, 8);
    // 申请内存存放位图的 bits
    si->slot_bitmap.bits = (uint8_t*)kmalloc(si->slot_bitmap.btmp_bytes_len);
    
    if(si->slot_bitmap.bits == NULL){
        kfree(si);
        lock_release(&swap_lock);
        PANIC("do_swapon: fail to kmalloc for bitmap");
    }

    bitmap_init(&si->slot_bitmap);

    ASSERT(dev_id <= MAX_SWAP_DEVICES && dev_id >= 1);
    
    si->dev_id = dev_id;
    swap_table[dev_id] = si;

    printk("do_swapon: %s enabled as swap device %d, slot count: %d\n", part->name, si->dev_id, si->slot_cnt);
    lock_release(&swap_lock);
}

// 目前的设计中，只要有正在使用的 slot，那么我们就拒绝卸载这个 swap 设备
// 在之后可以考虑这样优化他：先将设备设置为只读，只将内容从磁盘输出，然后引导用户去清空磁盘上的 slot
// 比如引导用户使用 kill 命令去终止那些正在使用该设备进行 swap 的进程
// 等到 slot 全部为空了再进行卸载，这么做需要额外考虑一下在 swapoff 的过程中，如果用户又重新 swapon 了这个设备该怎么办
// 这时可能需要重新恢复这个设备的写权限，让其可以重新写入数据
// 由于这个交互比较麻烦，目前先直接拒绝卸载，对于目前的使用场景来说已经足够了
void do_swapoff(struct partition* part) {
    lock_acquire(&swap_lock);
    
    int32_t dev_id = get_swap_info_by_part(part);
    if (dev_id < 0) {
        printk("swapoff: swap device not found\n");
        lock_release(&swap_lock);
        return;
    } 

    struct swap_info* si = swap_table[dev_id]; 

    // 只要还有数据，就拒绝卸载
    if (si->used_slots > 0) {
        printk("swapoff: %s is busy (%d slots in use). Clean up tasks or wait.\n", 
               part->name, si->used_slots);
        lock_release(&swap_lock);
        return;
    } 

    ASSERT(dev_id == si->dev_id);
    // 彻底释放资源
    swap_table[dev_id] = NULL;
    kfree(si->slot_bitmap.bits);
    kfree(si);
    printk("swapoff: %s unmounted successfully.\n", part->name);
    
    lock_release(&swap_lock);
}

// 返回编码后的 PTE 值 (slot_idx << 4 | dev_id << 1)
uint32_t alloc_swap_slot(int32_t* status) {
    bool has_dev = false;
    for (int i = 1; i <= MAX_SWAP_DEVICES; i++) {
        struct swap_info* si = swap_table[i];
        if (si == NULL) continue;
        has_dev =true;
        if(si->used_slots > si->slot_cnt){
            continue;
        }
        // printk("alloc slot\n");
        // 扫描 1 个空位
        // 再磁盘上找一个可用的 page slot
        int bit_idx = bitmap_scan(&si->slot_bitmap, 1);
        if (bit_idx != -1) {
            bitmap_set(&si->slot_bitmap, bit_idx, 1);
            // 构造 PTE：bit 0 是 Present(0)，bit 1-3 是 dev_id，高位是 index
            // 当 present 位为 0 时，PTE 的其他的几个属性位都会被直接忽略。
            // 因此我们可以直接复用这几个位来存储我们的 dev_id，但是这可能会覆盖掉我们原本的 RW 位
            // 这也不用担心，因为我们可以通过 VMA 来恢复这个权限位。
            // 防止上溢出，通常不容易发生，主要要检查的是下溢出，上溢出只是顺手检查
            ASSERT(si->used_slots+1>si->used_slots);
            si->used_slots++;
            *status = 0;
            return (uint32_t)((bit_idx << 4) | (si->dev_id << 1));
        }
    }
    if(has_dev){
        *status = -ENOMEM; // 交换空间全满
    } else {
        *status = -ENODEV; // 无交换设备
    }
    return 0;
}

void free_swap_slot(uint32_t pte_val) {
    uint8_t dev_id = (pte_val >> 1) & 0x07;
    uint32_t slot_idx = pte_val >> 4;
    
    struct swap_info* si = swap_table[dev_id];
    if (si) {
        bitmap_set(&si->slot_bitmap, slot_idx, 0); // 归还位图
        // 操作不当时，非常容易发生下溢出，需要检查
        ASSERT(si->used_slots-1 < si->used_slots); // 防止下溢
        si->used_slots--;
    }
}

/*
    使用 Clock 算法找到一个可以被踢出的页
    (0, 0)：最近未访问且干净。最高优先级，直接踢，代价最小。
    (0, 1)：最近未访问但脏。需要写磁盘，但反正它最近没人用。
    (1, 0)：最近访问过但干净。虽然它是干净的，但踢了它马上就会缺页。
    (1, 1)：最近访问过且脏。最后才考虑。
    优先找那些脏位为 0 的块，以便最大化 I/O 效率
    按理来说，最多 4 轮扫描一定能找到
*/
static struct page* pick_page_from_activate_list(void) {
    struct buddy_pool* pool = &user_pool;
    if (dlist_empty(&pool->activate_list)) return NULL;

    struct dlist_elem* ptr;
    struct dlist_elem* tail = &pool->activate_list.tail;
    
    // 第一轮扫描，寻找 (0, 0) 
    // 不修改任何标志位，只找最完美的牺牲者
    ptr = pool->activate_list.head.next;
    while (ptr != tail) {
        struct page* pg = member_to_entry(struct page, activate_tag, ptr);
        
        // 第一所有者为空的进程不应该在活跃队列中
        ASSERT(pg->first_owner!=NULL);

        uint32_t* pte_ptr = get_pte_ptr(pg->first_owner->pgdir, pg->first_vaddr);
        
        if (!(*pte_ptr & PG_A) && !(*pte_ptr & PG_D) && pg->ref_count==1) {
            goto found;
        }
        ptr = ptr->next;
    }

    // 第二轮扫描，寻找 (0, 1)，并将 A 位清零 
    // 如果没找到 (0, 0)，就找那些没被访问过但脏了的。
    // 顺便把扫过的 A 位都清 0，给下一轮创造 (0, 0)
    ptr = pool->activate_list.head.next;
    while (ptr != tail) {
        struct page* pg = member_to_entry(struct page, activate_tag, ptr);

        ASSERT(pg->first_owner!=NULL);

        uint32_t* pte_ptr = get_pte_ptr(pg->first_owner->pgdir, pg->first_vaddr);
        
        if (!(*pte_ptr & PG_A) && (*pte_ptr & PG_D) && pg->ref_count==1) {
            goto found;
        }
        // 没找到，但把 A 位抹掉，给予“降级”
        *pte_ptr &= ~PG_A;
        asm volatile("invlpg (%0)" : : "r"(pg->first_vaddr) : "memory");
        ptr = ptr->next;
    }

    // 第三轮扫描，重复第一轮
    // 因为第二轮清了 A 位，现在肯定能找到 (0, 0) 了
    ptr = pool->activate_list.head.next;
    while (ptr != tail) {
        struct page* pg = member_to_entry(struct page, activate_tag, ptr);

        ASSERT(pg->first_owner!=NULL);

        uint32_t* pte_ptr = get_pte_ptr(pg->first_owner->pgdir, pg->first_vaddr);
        
        if (!(*pte_ptr & PG_A) && !(*pte_ptr & PG_D) && pg->ref_count==1) {
            goto found;
        }
        ptr = ptr->next;
    }

    // 第四轮扫描，保底方案
    // 找任何一个 A=0 的（此时必然存在 A=0, D=1 的页，这是最开始(1,1)的页）
    ptr = pool->activate_list.head.next;
    while (ptr != tail) {
        struct page* pg = member_to_entry(struct page, activate_tag, ptr);

        ASSERT(pg->first_owner!=NULL);

        uint32_t* pte_ptr = get_pte_ptr(pg->first_owner->pgdir, pg->first_vaddr);
        if (!(*pte_ptr & PG_A) && pg->ref_count==1) goto found;
        ptr = ptr->next;
    }

    return NULL;

found:
    return member_to_entry(struct page, activate_tag, ptr);
}

// 挑选一个页面并将其置换到磁盘，释放一个物理页框
// 成功释放的物理页起始地址
static void* swap_out(void) {
    lock_acquire(&swap_lock);
    // 使用 Clock 算法找到牺牲者
    struct page* pg = pick_page_from_activate_list();
    if (pg == NULL) {
        lock_release(&swap_lock);
        return NULL; // 实在是没页可踢了（比如全是共享页或内核页）
    }

    uint32_t vaddr = pg->first_vaddr;
    struct task_struct* owner = pg->first_owner;

    // 我们是可以将自己的页置换到磁盘的，不一定非要置换其他进程的页
    // ASSERT(pg->first_owner != get_running_task_struct());

#ifdef DEBUG_SWAP
    printk("swap_out: swap out %s for %s\n",pg->first_owner->name,get_running_task_struct()->name);
#endif
    uint32_t* pte_ptr = get_pte_ptr(owner->pgdir, vaddr);
    
    // 记录下该页的物理地址，最后返回它
    void* phys_addr = (void*)(*pte_ptr & 0xfffff000);

    bool is_dirty = (*pte_ptr & PG_D);
    
    // struct vm_area* vma = find_vma(owner, vaddr);
    // 扩大了写回的条件，导致了我们对于磁盘容量的要求更高了，原本只需要一个 sdb1 (3901 个页 slot) 就能在4MB环境下运行 tcc_emu0 测试
    // 现在需要更多的空间才能进行了，大概需要 sdb1 (3901 slots) + sdb5 (2011 slots) + sdb6 (2893 slots) + sdb7 (1633 slots) 才能运行完毕
    // bool is_writable = (vma->vma_flags & VM_WRITE); 

    // 为了简单起见，我们直接使用页上的 W 位来判断是否可写，可以少一次 vma 链表的遍历
    // 如果后期因为 swap 导致 0 地址页错误了，那么可以尝试将此处的 is_writable 改成用 vma 判断
    bool is_writable = (*pte_ptr & PG_RW_W);

    // 检查脏位 (PG_D)
    // 如果页面是脏的，或者它从未被换出过，则必须写入磁盘
    // is_writable 会覆盖所有可写的地址范围，包括堆段和栈段以及某些 mmap 出来的区域，由于他们是孤本，在磁盘没有任何备份
    // 在动态链接的情况下，会引发很多莫名其妙的错误，因此保险起见，对于这样的页我们也都要写回，防止丢数据
    // 静态链接时，程序的布局比较固定，也不存在很复杂的运行时加载操作，因此我们只判断 is_dirty
    // 如果后期因为 swap 导致 0 地址页错误了，可以尝试取消这个对于静态链接程序的优化
    // 如果页面是干净的 (D=0)，说明磁盘上的数据和内存一致，直接跳过写入
    if (is_dirty || (owner->is_dyn_link && is_writable)) {
        // 匿名脏页，直接写到 Swap 分区
        // 对于文件映射脏页
        // 因为我们目前只支持 MAP_PRIVATE，私有映射的脏页不应该写回原文件！
        // 它们一旦变脏，就变成了“写时复制”后的私有数据，应该当做匿名页处理，写到 Swap。
        int32_t status = 0;
        uint32_t swap_pte = alloc_swap_slot(&status);  
        if (status < 0) {
            if(status == -ENOMEM){
                printk("swap_out: fail to alloc_swap_slot, swap space exhausted!!!\n");
            }else if(status == -ENODEV){
                printk("swap_out: fail to alloc_swap_slot, you may need a swap device!!!\n");
            } else {
                // 先 panic ，之后再进一步处理
                PANIC("swap_out: fail to alloc_swap_slot, unknown error!");
            }
            lock_release(&swap_lock);
            
            return NULL; 
        }

        // 调用 swap_write 写入磁盘
        // 这里的 buf 需要是虚拟地址，我们需要将物理地址转换成虚拟地址
        // 由于用户传入的物理地址可能位于 1GB 区域以上，因此不要直接用+0xc000000的方式进行虚拟地址的转换
        // 我们直接使用 kmap 来建立一个临时映射
        // 将这个物理地址临时绑定到一个高端128MB 的虚拟地址上，操作完了再释放这个虚拟地址
        void* kaddr = kmap((uint32_t)phys_addr);
        swap_write(swap_pte, kaddr);
        kunmap(kaddr);
        
        // 我们直接将刚刚构造好的新 pte 条目存到相应的 pte 中
        // 这样的话，在访问这个虚拟地址时，硬件会发现 P 位为 0 并触发缺页中断
        // 然后将流程给到 swap_page 函数中，swap_page 会将这个 pte 的内容和触发缺页的虚拟地址传到 swap_in 函数中
        // 然后 swap_in 函数会申请新的物理页，然后根据 *pte_ptr 中的页 slot 号到磁盘的相应位置将数据换入内存
        // 之后建立新申请的物理页和触发页错误的虚拟页之间的映射  
        *pte_ptr = swap_pte; // 页表存 Swap 索引
    } else {
        // 对于只读类型的页，我们直接把页表项置空，等访问时触发缺页中断逻辑
        // 然后让 swap_page 函数来处理
        // 这会让我们得到一个神奇的特性，如果我们目前置换的页不是一个脏页，那么即使没有 swap 设备我们的 swap 操作也能成功
        // 如果一个程序整体没有脏页，那么在没有 swap 设备的情况下即使将整个程序 swap 出去也都不会产生问题
        *pte_ptr = 0;
    }

    ASSERT(pg->ref_count == 1);

    // 维护 struct page 元数据
    // 清除该页的映射关系，因为它已经不再属于任何进程了
    // 引用计数应该归零（在 pick 逻辑里我们已经保证了 ref_count == 1）
    // pfree 会替我们完成这些工作
    pfree((uint32_t)phys_addr);

    // 刷 TLB
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");

    lock_release(&swap_lock);
#ifdef DEBUG_SWAP
    printk("swap_out: unbind and pfree paddr(0x%x) and vaddr(0x%x)\n", phys_addr, vaddr);
#endif
    // 返回这个可以被重新使用的物理地址
    return phys_addr;
}

// 从交换分区换入页面
// pte_ptr 缺页地址对应的页表项指针
// page_vaddr 缺页的虚拟起始地址（4KB对齐）
// vma 所在的虚拟内存区域，用于恢复权限
static bool swap_in(uint32_t* pte_ptr, uint32_t page_vaddr, struct vm_area* vma) {
    lock_acquire(&swap_lock);
    uint32_t pte_val = *pte_ptr;
    
    void* page_paddr = NULL;

    // 使用和 swap_page 函数里类似的尽力而为的分配
    while (1) {
        page_paddr = palloc(&user_pool);
        if (page_paddr != NULL) {
            break; 
        }

        // 尝试腾出一个页
        if (swap_out() == NULL) {
            // 物理页全被锁定或全是内核页，实在无法置换
            lock_release(&swap_lock);
            // 目前先 panic，防止内核跑飞
            printk("swap_in: OOM - no page can be swapped out!\n");
            return false; 
        }
        // swap_out 成功后，下一轮循环会再次尝试 palloc
    }

    ASSERT(page_paddr != NULL)

    // 从磁盘换入数据，利用 kmap 来防止用户的地址是一个大于 1GB 的地址
    void* kaddr = kmap((uint32_t)page_paddr);
    swap_read(pte_val, kaddr);
    kunmap(kaddr);

    // 释放磁盘槽位
    free_swap_slot(pte_val);

    // mapping_v2p 会帮我们处理：page_table_add、元数据设置、加入 activate_list
    mapping_v2p(page_vaddr, (uint32_t)page_paddr);

    // 根据 VMA 补全权限位
    // mapping_v2p 默认可能开了写权限，如果 VMA 是只读的，这里记得修正一下
    uint32_t attr = (vma->vma_flags & VM_WRITE) ? PG_RW_W : PG_RW_R;
    *pte_ptr = (uint32_t)page_paddr | PG_P_1 | PG_US_U | attr;
    asm volatile("invlpg (%0)" : : "r"(page_vaddr) : "memory");
    lock_release(&swap_lock);
#ifdef DEBUG_SWAP
    printk("swap_in: bind paddr(0x%x) with vaddr(0x%x)\n",page_paddr,page_vaddr);
#endif
    return true;
}

static void swap_read(uint32_t pte_val, void* buf) {
    uint8_t dev_id = (pte_val >> 1) & 0x07;
    uint32_t slot_idx = pte_val >> 4;
    struct swap_info* si = swap_table[dev_id];

    // 一个 Slot 占 8 个扇区 (4KB / 512B)
    uint32_t logic_lba = slot_idx * 8; 

    partition_read(si->part, logic_lba, buf, 8);
}

static void swap_write(uint32_t pte_val, void* buf) {
    uint8_t dev_id = (pte_val >> 1) & 0x07;
    uint32_t slot_idx = pte_val >> 4;
    struct swap_info* si = swap_table[dev_id];
    
#ifdef DEBUG_SWAP
    printk("swap_write: write to dev %s\n", si->part->name);
#endif

    // 一个 Slot 占 8 个扇区 (4KB / 512B)
    uint32_t logic_lba = slot_idx * 8; 

    partition_write(si->part, logic_lba, buf, 8);
}