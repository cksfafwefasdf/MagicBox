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

	dlist_init(&child_thread->vma_list);

	// 我们现在使用vma链表来管理用户进程的虚拟地址空间，因此位图可以取消了 
	// uint32_t bitmap_pg_cnt = DIV_ROUND_UP((USER_STACK_BASE-USER_VADDR_START)/PG_SIZE/8,PG_SIZE);

	// 在 linux 的标准中，父子进程的名称是完全相同的
	// 只有到execv阶段才会改变名称
	// strcat(child_thread->name,"_fork");

	return 0;
}

static int32_t build_child_stack(struct task_struct* child) {
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

static int32_t copy_process(struct task_struct* child_thread,struct task_struct* parent_thread){
	
	void* buf_page = get_kernel_pages(1);

	if(buf_page==NULL){
		return -1;
	}

	if(copy_pcb_vaddrbitmap_stack0(child_thread,parent_thread) == -1){
		return -1;
	}

	if (!copy_vma_list(parent_thread, child_thread)) {
        // 如果 VMA 拷贝失败（比如 kmalloc 失败），需要清理已分配资源
		// 在此我们先 PANIC 便于调试
		PANIC("failed to copy_vma_list!");
        return -1; 
    }
	
	
	child_thread->pgdir = create_page_dir();
	
	if(child_thread->pgdir==NULL){
		return -1;
	}
	
	// copy_body_stack3(child_thread,parent_thread,buf_page);
	
	copy_page_tables(parent_thread,child_thread,buf_page);
	
	build_child_stack(child_thread);
	update_f_cnts(child_thread);
	mfree_page(PF_KERNEL,buf_page,1);
#ifdef DEBUG_PG_FAULT
	printk("copy_process::: copy_process done!\n");
#endif
	return 0;
}

pid_t sys_fork(){
	
	struct task_struct* parent_thread = get_running_task_struct();
	
	struct task_struct* child_thread = get_kernel_pages(1);

	if(child_thread==NULL){
		return -1;
	}

	ASSERT(INTR_OFF == intr_get_status()&&parent_thread->pgdir!=NULL);
	if(copy_process(child_thread,parent_thread) == -1){
		return -1;
	}
	

	ASSERT(!dlist_find(&thread_ready_list,&child_thread->general_tag));
	dlist_push_back(&thread_ready_list,&child_thread->general_tag);
	ASSERT(!dlist_find(&thread_all_list,&child_thread->all_list_tag));
	dlist_push_back(&thread_all_list,&child_thread->all_list_tag);
	return child_thread->pid;
}
