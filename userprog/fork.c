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
#include <syscall.h>
#include <wait_exit.h>

extern void intr_exit(void); // defined in  kernel.s
static int32_t copy_pcb_vaddrbitmap_stack0(struct task_struct* child_thread,struct task_struct* parent_thread){
	
	// memcpy(child_thread,parent_thread,PG_SIZE);

	// 由于我们现在已经把内核栈拆出去了
	// 因此不要再拷贝一整页了，防止覆盖新 PCB 的结构
	// 之前内核栈在pcb中时，直接复制一整页可以把父进程的内核栈信息也复制过来
	// 但现在这么做复制不过来了，因此没必要复制那么多了
	memcpy(child_thread,parent_thread,sizeof(struct task_struct));

	// 上面的拷贝操作把父进程的打开文件表的指针都拷贝了过来，我们先把这个指针置为 NULL
	child_thread->file_table = NULL; 

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

static int32_t build_child_stack(struct task_struct* child, void* user_stack, int (*fn)(void *fnarg), void *arg, void (*thread_restorer)(void)) {
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
        child_intr->ebp = (uint32_t) user_stack; 
    }

    // 根据 fn 进一步构建中断栈给返回到用户态时使用
    if(fn != NULL){ // 有 fn，按照构造内核线程的流程构造
        // 定位子进程的用户空间栈顶，并预埋一个用户态的退出系统调用
        // 当用户函数 fn 执行完 ret 时，必须让它弹进一个能引发 exit 的地方
        uint32_t* ustack = (uint32_t*)user_stack;
        
        ustack--;
        *ustack = (uint32_t)arg; // 压入传给 fn 的参数

        // 压入 fn 的返回地址 exit
        // 由于是在用户态下，执行完 fn 后的返回地址
        // 因此压入的必须是用户态下的系统调用
        ustack--;
        *ustack = (uint32_t)thread_restorer; 

        // 强行将子进程的用户栈和 EIP 塞进中断栈
        child_intr->esp = (void*)ustack;
        child_intr->ebp = (uint32_t)ustack;
        // eip 给出的函数类型的返回值是 void，但是我们提供的是 int，因此先强转一下
        child_intr->eip = (void (*)(void))fn; // 目标直接指向用户态函数
    } 

    // 构建 thread_stack 供 switch_to 使用
    uint32_t* ret_addr = (uint32_t*)child_intr - 1;
    // 如果 fn 不为空，那么 intr_exit 后就会返回到 fn 中
    // 否则返回到的是和 parent_intr 一样的 eip 处（因为我们上面执行了一个 memcpy）
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
        global_fd = thread->file_table->fd_table[local_fd].global_fd_idx;
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

static int32_t copy_process(uint32_t flags, struct task_struct* child_thread, struct task_struct* parent_thread){
    
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
        // printk("sys_clone: share vm\n");
        // 线程模式，直接指向父进程的 mm 结构，实现内存完美共享
        child_thread->mm = parent_thread->mm;
        
        lock_acquire(&parent_thread->mm->mm_lock);
        child_thread->mm->mm_users++; 
        lock_release(&parent_thread->mm->mm_lock);
                
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
        init_mm_struct(child_thread->mm);

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
        // printk("sys_clone: share files\n");
        // 线程模式，直接使用父进程的文件表指针
		// 在这种情况下，子进程关闭一个文件父进程会受到影响
        child_thread->file_table = parent_thread->file_table;

        child_thread->file_table->ref_cnt++;
        
        // 既然 fd_table 是同一个，里面的 global_fd 没变，整体没有产生新的局部表项，
        // 因此不需要遍历增加全局 file 的 f_count。
    } else {
        // 传统进程模式 (Fork)，使用原本的逻辑更新计数
		// 在这种情况下，子进程关闭一个文件，不会对父进程产生影响
		child_thread->file_table = kmalloc(sizeof(struct file_table));

        memcpy(child_thread->file_table, parent_thread->file_table, sizeof(struct file_table));

        init_file_table(child_thread->file_table);
		
        update_f_cnts(child_thread);
    }
    
    mfree_page(PF_KERNEL, buf_page, 1);
#ifdef DEBUG_PG_FAULT
    printk("copy_process::: copy_process done with flags: 0x%x!\n", flags);
#endif
    return 0;
}

// 在 VM 共享的情况下，内核态线程和用户进程会共享 虚拟地址空间
// 同时，也会共享 esp，这样的话子线程的 call 操作甚至会影响到父进程的 esp
// 这显然有问题，因此我们需要传入一个 user_stack 来将二者分离
// fn 是内核线程的业务函数的逻辑，如果不传入该参数的话，进程会返回到 clone 函数执行完的下一个地址处
// 其实这个 fn 为空的话，整体的运行流程就是原本的 fork 的运行流程，如果带有 fn 的话，clone 完会进入到 fn 中
// thread_restorer 类似于 sig_restorer, 用于 LWP 的退出处理
pid_t sys_clone(uint32_t flags, void* user_stack, int (*fn)(void *fnarg), void *arg, void (*thread_restorer)(void)) {
    struct task_struct* parent_thread = get_running_task_struct();
    struct task_struct* child_thread = get_kernel_pages(1); // 申请一页作为 PCB 容器

    if(child_thread == NULL){
        return -1;
    }

    ASSERT(INTR_OFF == intr_get_status() && parent_thread->mm->pgdir != NULL);
    
    // 带上 flags 运行
    if(copy_process(flags, child_thread, parent_thread) < 0){
        mfree_page(PF_KERNEL, child_thread, 1);
        return -1;
    }

    // 构建子任务的内核栈
    if(build_child_stack(child_thread, user_stack, fn, arg, thread_restorer)<  0){
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
    return sys_clone(0, NULL, NULL, NULL, NULL); 
}
