#include <stdint.h>
#include <thread.h>
#include <string.h>
#include <process.h>
#include <debug.h>
#include <interrupt.h>
#include <dlist.h>
#include <fork.h>
#include <stdio-kernel.h>
#include <vgacon.h>
#include <pipe.h>
#include <vma.h>
#include <file_table.h>
#include <inode.h>
#include <ide.h>
#include <swap.h>

extern void intr_exit(void); // defined in  kernel.s
static int32_t copy_pcb_vaddrbitmap_stack0(struct task_struct* child_thread,struct task_struct* parent_thread){
	
	// memcpy(child_thread,parent_thread,PG_SIZE);

	// 由于我们现在已经把内核栈拆出去了
	// 因此不要再拷贝一整页了，防止覆盖新 PCB 的结构
	// 之前内核栈在pcb中时，直接复制一整页可以把父进程的内核栈信息也复制过来
	// 但现在这么做复制不过来了，因此没必要复制那么多了
	memcpy(child_thread,parent_thread,sizeof(struct task_struct));

	// 上面的拷贝操作把父进程的打开文件表的指针都拷贝了过来，我们先把这个指针置为 NULL
	child_thread->fd_table = NULL; 

	// 重新分配子进程的独立内核栈
    child_thread->kstack_pages = get_kernel_pages(KERNEL_THREAD_STACK_PAGES);

	child_thread->pid = fork_pid();
	child_thread->elapsed_ticks = 0;
	child_thread->status = TASK_READY;
	child_thread->priority = parent_thread->priority;
	child_thread->ticks = child_thread->priority;
	child_thread->parent_pid = parent_thread->pid;
	child_thread->pgrp = parent_thread->pgrp; // 子进程继承父进程的组id
	child_thread->general_tag.prev = child_thread->general_tag.next = NULL;
	child_thread->all_list_tag.prev = child_thread->all_list_tag.next = NULL;
	// 子进程继承父进程的工作目录
	child_thread->pwd = parent_thread->pwd;

	// 增加一下引用计数，以便和exit的close对称
	inode_open(get_part_by_rdev(child_thread->pwd->i_dev),child_thread->pwd->i_no);

	// dlist_init(&child_thread->mm->vma_list);

	// 我们现在使用vma链表来管理用户进程的虚拟地址空间，因此位图可以取消了 
	// uint32_t bitmap_pg_cnt = DIV_ROUND_UP((USER_STACK_BASE-USER_VADDR_START)/PG_SIZE/8,PG_SIZE);

	// 在 linux 的标准中，父子进程的名称是完全相同的
	// 只有到execv阶段才会改变名称
	// strcat(child_thread->name,"_fork");

	return 0;
}

static int32_t build_child_stack(struct task_struct* child, void* user_stack) {
    struct task_struct* parent = get_running_task_struct();

    // 定位父进程进入内核时的中断栈位置
    // 父进程的 self_kstack 此时正处于内核态某个位置，
    // 但中断栈(intr_stack)始终在它 kstack_pages + 8KB 的顶端
    struct intr_stack* parent_intr = (struct intr_stack*)((uint32_t)parent->kstack_pages + KERNEL_THREAD_STACK- sizeof(struct intr_stack));

    // 定位子进程中断栈位置
    struct intr_stack* child_intr = (struct intr_stack*)((uint32_t)child->kstack_pages + KERNEL_THREAD_STACK - sizeof(struct intr_stack));

    // 拷贝中断上下文
    memcpy(child_intr, parent_intr, sizeof(struct intr_stack));
    child_intr->eax = 0; // 子进程返回 0

	if ((child->mm == parent->mm) && user_stack != NULL) {
        child_intr->esp = user_stack; 
    }

    // 构建 thread_stack 供 switch_to 使用
    uint32_t* ret_addr = (uint32_t*)child_intr - 1;
    *ret_addr = (uint32_t)intr_exit;

    // 最终 self_kstack 指向 thread_stack 的栈顶（即 ret_addr 往下 4 个寄存器）
    child->self_kstack = (uint32_t*)child_intr - 5; 
    return 0;
}

// 原本我们的这个函数是 update_inode_open_cnts，修改的是inode的打开计数
// 但是当我们引入 f_count 后，这个函数显然就不合理了
// 因为 fork 后的子进程会继承父进程的局部打开文件表
// 这意味着子进程和父进程相应的局部打开文件表表项会指向同一个全局打开文件表的表项
// 根据 f_count 的定义我们知道，它用于标志有多少个局部打开文件表项指向它
// 因此 fork 中，我们需要增加的不是 i_open_cnt 而是 f_count
// i_open_cnt 标志着有多少个全局打开文件表项指向同一个 inode
// fork 操作并没有新增全局打开文件表项，因此增加 i_open_cnt 是不合理的
static void update_f_cnts(struct task_struct* thread) {
    // 原来的代码是从3开始的！
	// 现在我们要改成从 0 开始，因为 stdin, stdout, stderr 也是需要增加引用计数的！
	// 我们将 console 和 tty 都给文件话了，不能再像原来那样略过它们了
    int32_t local_fd = 0, global_fd = 0; 
    while(local_fd < MAX_FILES_OPEN_PER_PROC) {
        global_fd = thread->fd_table[local_fd].global_fd_idx;
        if(global_fd != -1) {
            struct file* f = &file_table[global_fd];
            // 必须增加引用计数，否则父子进程共享 FD 会出大问题
            f->f_count++;

			// 管道特有逻辑，维护端点存活计数
            // 如果是管道，必须根据读写标志位增加对应的 reader/writer 计数
            if (f->fd_inode->i_type == FT_PIPE) {
                struct pipe_inode_info* pii = (struct pipe_inode_info*)&f->fd_inode->pipe_i;
                if (f->fd_flag & O_RDONLY) pii->reader_count++;
                if (f->fd_flag & O_WRONLY) pii->writer_count++;
            }
        }
        local_fd++;
    }
}

static int32_t copy_process(uint32_t flags, void* user_stack, struct task_struct* child_thread, struct task_struct* parent_thread){
    
    void* buf_page = get_kernel_pages(1);
    if(buf_page == NULL){
        return -1;
    }

    // 先拷贝 PCB 基本结构
    if(copy_pcb_vaddrbitmap_stack0(child_thread, parent_thread) == -1){
        mfree_page(PF_KERNEL, buf_page, 1);
        return -1;
    }

    /// 处理共享虚拟地址空间 (CLONE_VM)
    if (flags & CLONE_VM) {
        // 线程模式，直接指向父进程的 mm 结构，实现内存完美共享
        child_thread->mm = parent_thread->mm;
        
        // 在这里增加使用该 mm 的线程计数
		// 此处不需要用 lock，因为当前操作是在关中断下进行的
		// 即使是 SMP 的情况下，也不需要用，因为子进程还没有进入 ready 队列呢
		// 只有父进程对这个 mm 有完全的控制权
        child_thread->mm->mm_users++; 
        
        // 因为共享，不需要 copy_vma_list，也不需要 create_page_dir 和 copy_page_tables
    } else {
        // 传统进程模式 (Fork)，独立分配虚拟地址空间
		// 在结构体拷贝完成后，再为子进程分配属于它自己的、独立的 mm 结构
    	// 这样就洗掉了刚才被 memcpy 错误覆盖过来的父进程 mm 指针
        child_thread->mm = kmalloc(sizeof(struct mm_struct));
        if (child_thread->mm == NULL) {
            mfree_page(PF_KERNEL, buf_page, 1);
            return -1;
        }
        dlist_init(&child_thread->mm->vma_list);
        lock_init(&child_thread->mm->mm_lock);

        if (!copy_vma_list(parent_thread, child_thread)) {
			// 如果 VMA 拷贝失败（比如 kmalloc 失败），需要清理已分配资源
			// 在此我们先 PANIC 便于调试
            PANIC("failed to copy_vma_list!");
            return -1; 
        }
        
        child_thread->mm->pgdir = create_page_dir();
        if(child_thread->mm->pgdir == NULL){
            kfree(child_thread->mm);
            mfree_page(PF_KERNEL, buf_page, 1);
            return -1;
        }
        
        // 复制多级页表并设为只读
        copy_page_tables(parent_thread, child_thread, buf_page);
    }

    // 处理打开文件表 (CLONE_FILES)
    if (flags & CLONE_FILES) {
        // 线程模式，直接使用父进程的文件表指针
		// 在这种情况下，子进程关闭一个文件父进程会受到影响
        child_thread->fd_table = parent_thread->fd_table;
        
        // 既然 fd_table 是同一个，里面的 global_fd 没变，整体没有产生新的局部表项，
        // 因此不需要遍历增加全局 file 的 f_count。
    } else {
        // 传统进程模式 (Fork)，使用原本的逻辑更新计数
		// 在这种情况下，子进程关闭一个文件，不会对父进程产生影响
		child_thread->fd_table = kmalloc(sizeof(struct fd_entry)*MAX_FILES_OPEN_PER_PROC);
		memcpy(child_thread->fd_table , parent_thread->fd_table, sizeof(struct fd_entry)*MAX_FILES_OPEN_PER_PROC);
        update_f_cnts(child_thread);
    }
    
    // 构建子任务的内核栈
    build_child_stack(child_thread, user_stack);
    
    mfree_page(PF_KERNEL, buf_page, 1);
#ifdef DEBUG_PG_FAULT
    printk("copy_process::: copy_process done with flags: 0x%x!\n", flags);
#endif
    return 0;
}

/*
    TODO:   (1) 在 clone 和 exit 中添加对进程 fd_table 的引用计数的操作
                以便实现共享退出时的保护，防止父进程或子线程在退出时无脑的对文件进行 close 和释放 fd_table
            (2) 在 exit 中添加对于 mm 结构体释放的保护，先检查计数为 0 了再释放
                防止子进程或者父进程退出时无脑的释放 mm 结构体中的各个页以及页表空间
        注意:   在 clone 一个轻量级进程 (LWP) 时，父进程 A 和 LWP 进程 B 共享 VM，此时如果子进程 LWP 又 fork 了一个进程 C
                由于我们的页表拷贝操作会默认把页面设为只读，因此此时 A B C 会共享页面，当 C 发生写操作时，会触发 COW
                此时 C 会自己复制一份然后搬走，但是此时 A 和 B 指向的页面仍然是只读的
                如果此时 B 再 发生了写操作，那么 COW 这时候又会让 B 自己复制一份自己搬出去，这样的话父子进程就不共享 VM 了！
                这与我们的初衷不符，一个可能有效的解决方法是让 A 和 B 共享同一个 page 结构体的计数
                也就是说 B 虽然和 A 使用同一块物理页面，但是由于它是直接共享的 mm 结构体而不是走的 copy_page_table
                来拷贝的页表，因此相应物理页的引用计数并没有增加，进而只有 A 和 B 进程的情况下，触发 COW 时，此时 page 的引用计数为 1
                我们 write_protect 会直接将相应的页的权限设为可写，而不会进行拷贝，因此这样一来父子进程依然还是共享同一物理页的
                但是问题出在 A B C 同时存在时，如果 B 写了物理页，导致了 COW，此时引用计数是 2，还不足以直接将页面设为可写
                我们只为 B 进行拷贝，那么 A 有可能就不和 B 共享同一个物理页了！我们需要检查一下在这种情况下，B 触发 COW 后
                二者是否还共享同一个物理页，按理来说应该也还是共享的，因为 A 和 B 共享页表，B 将一个虚拟地址绑定到新的物理页后
                A 自动的也就会发生同样的变化了，所以按理来说应该没有太大问题
*/

// 在 VM 共享的情况下，内核态线程和用户进程会共享 虚拟地址空间
// 同时，也会共享 esp，这样的话子线程的 call 操作甚至会影响到父进程的 esp
// 这显然有问题，因此我们需要传入一个 user_stack 来将二者分离

// 由于 clone 在共享 VM 的情况下和 exit 之间的配合还没有完全写好
// 因此 clone 先不导出出去了，先只作为一个 static 的函数专门给 fork 用
static pid_t sys_clone(uint32_t flags, void* user_stack) {
    struct task_struct* parent_thread = get_running_task_struct();
    struct task_struct* child_thread = get_kernel_pages(1); // 申请一页作为 PCB 容器

    if(child_thread == NULL){
        return -1;
    }

    ASSERT(INTR_OFF == intr_get_status() && parent_thread->mm->pgdir != NULL);
    
    // 带上 flags 运行
    if(copy_process(flags, user_stack, child_thread, parent_thread) == -1){
        mfree_page(PF_KERNEL, child_thread, 1);
        return -1;
    }
    
    // 放入就绪队列和全局队列
    ASSERT(!dlist_find(&thread_ready_list, &child_thread->general_tag));
    dlist_push_back(&thread_ready_list, &child_thread->general_tag);
    ASSERT(!dlist_find(&thread_all_list, &child_thread->all_list_tag));
    dlist_push_back(&thread_all_list, &child_thread->all_list_tag);
    
    return child_thread->pid;
}

// fork 纯粹是 clone 的一个特化封装（完全不带共享标志）
pid_t sys_fork(){
    return sys_clone(0, NULL); 
}
