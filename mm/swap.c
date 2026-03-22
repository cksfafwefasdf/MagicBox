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

// 使用位图来磁盘管理swap分区中有哪些空间是空闲的
struct swap_info {
    struct bitmap slot_bitmap; // 管理磁盘槽位的分配情况
    uint32_t start_lba; // 交换区在磁盘上的起始扇区
    uint32_t slot_cnt; // 总共有多少个 4KB 槽位
};

static bool is_vaddr_mapped(struct task_struct* task UNUSED, uint32_t vaddr) {
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

// 该函数对应两者情况
// 懒加载/交换：内核分配物理页。
// 非法访问：访问了完全没有映射、或者不属于用户空间（如访问内核空间地址）的内存
void swap_page(uint32_t err_code,void* err_vaddr){
	enum intr_status _old =  intr_disable();
#ifdef DEBUG_SWAP
	printk("swap_page:::err_code: %d, err_vaddr: %x\n", err_code, err_vaddr);
#endif
	struct task_struct* cur = get_running_task_struct();
	uint32_t vaddr = (uint32_t)err_vaddr;

	// 硬件给出的 vaddr 可能是 0xbffffabc，我们需要把它对齐到 0xbffff000
    uint32_t page_vaddr = vaddr & 0xfffff000;

    // 防止swap_page递归重入，掩盖错误的第一现场
    // 如果报错地址小于 0x1000（第一页），通常不是懒加载，而是代码 Bug，
    // 主要是针对内核线程的限制，用户进程会发信号自己退出
    if (vaddr < 0x1000&&cur->pgdir==NULL) {
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

    // 调用pfree减去老物理页的引用计数
    // 变成0时会自动释放，但是在此处应该不会变成0
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
#ifdef DEBUG_SWAP
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

    // printk("COW Done: vaddr %x now mapped to pa %x\n", vaddr, *pte & 0xfffff000);

	intr_set_status(_old);
}