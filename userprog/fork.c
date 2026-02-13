#include "stdint.h"
#include "thread.h"
#include "string.h"
#include "process.h"
#include "debug.h"
#include "file.h"
#include "interrupt.h"
#include "dlist.h"
#include "fork.h"
#include "stdio-kernel.h"
#include "print.h"
#include "pipe.h"

extern void intr_exit(void); // defined in  kernel.s
static int32_t copy_pcb_vaddrbitmap_stack0(struct task_struct* child_thread,struct task_struct* parent_thread){
	
	memcpy(child_thread,parent_thread,PG_SIZE);

	child_thread->pid = fork_pid();
	child_thread->elapsed_ticks = 0;
	child_thread->status = TASK_READY;
	child_thread->ticks = child_thread->priority;
	child_thread->parent_pid = parent_thread->pid;
	child_thread->pgrp = parent_thread->pgrp; // 子进程继承父进程的组id
	child_thread->general_tag.prev = child_thread->general_tag.next = NULL;
	child_thread->all_list_tag.prev = child_thread->all_list_tag.next = NULL;
	block_desc_init(child_thread->u_block_desc);

	uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000-USER_VADDR_START)/PG_SIZE/8,PG_SIZE);
	void* vaddr_btmp = get_kernel_pages(bitmap_pg_cnt);
	
	// printk("Child PCB: %x, Bitmp Vaddr: %x, Phys: %x\n", 
    //    child_thread, vaddr_btmp, addr_v2p(vaddr_btmp));
	
	memset(vaddr_btmp,0,bitmap_pg_cnt*PG_SIZE);
	child_thread->userprog_vaddr.vaddr_bitmap.bits = vaddr_btmp;
	// ASSERT(strlen(child_thread->name)<11);
	strcat(child_thread->name,"_fork");
	return 0;
}


static int32_t build_child_stack(struct task_struct* child_thread){
	struct intr_stack* intr_0_stack = (struct intr_stack*)((uint32_t)child_thread+PG_SIZE-sizeof(struct intr_stack));
	intr_0_stack->eax = 0;

	uint32_t* ret_addr_in_thread_stack = (uint32_t*) intr_0_stack - 1;
	uint32_t* esi_ptr_in_thread_stack = (uint32_t*) intr_0_stack - 2;
	uint32_t* edi_ptr_in_thread_stack = (uint32_t*) intr_0_stack - 3;
	uint32_t* ebx_ptr_in_thread_stack = (uint32_t*) intr_0_stack - 4;
	uint32_t* ebp_ptr_in_thread_stack = (uint32_t*) intr_0_stack - 5;
	*ret_addr_in_thread_stack = (uint32_t)intr_exit;

	*ebp_ptr_in_thread_stack = *ebx_ptr_in_thread_stack=*edi_ptr_in_thread_stack=*esi_ptr_in_thread_stack=0;

	child_thread->self_kstack = ebp_ptr_in_thread_stack;
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
        global_fd = thread->fd_table[local_fd];
        if(global_fd != -1) {
            struct file* f = &file_table[global_fd];
            // 必须增加引用计数，否则父子进程共享 FD 会出大问题
            f->f_count++;

			// 管道特有逻辑，维护端点存活计数
            // 如果是管道，必须根据读写标志位增加对应的 reader/writer 计数
            if (f->fd_inode->di.i_type == FT_PIPE) {
                struct pipe* p = (struct pipe*)f->fd_inode->di.i_pipe_ptr;
                if (f->fd_flag & O_RDONLY) p->reader_count++;
                if (f->fd_flag & O_WRONLY) p->writer_count++;
            }
        }
        local_fd++;
    }
}

uint32_t test_global_data = 0;

static int32_t copy_process(struct task_struct* child_thread,struct task_struct* parent_thread){
	
	void* buf_page = get_kernel_pages(1);

	if(buf_page==NULL){
		return -1;
	}

	if(copy_pcb_vaddrbitmap_stack0(child_thread,parent_thread)==-1){
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
	printk("copy_process::: copy_process done!\n");
	return 0;
}

pid_t sys_fork(){
	
	struct task_struct* parent_thread = get_running_task_struct();
	
	struct task_struct* child_thread = get_kernel_pages(1);

	if(child_thread==NULL){
		return -1;
	}

	ASSERT(INTR_OFF == intr_get_status()&&parent_thread->pgdir!=NULL);
	if(copy_process(child_thread,parent_thread)==-1){
		return -1;
	}
	

	ASSERT(!dlist_find(&thread_ready_list,&child_thread->general_tag));
	dlist_push_back(&thread_ready_list,&child_thread->general_tag);
	ASSERT(!dlist_find(&thread_all_list,&child_thread->all_list_tag));
	dlist_push_back(&thread_all_list,&child_thread->all_list_tag);
	return child_thread->pid;
}

