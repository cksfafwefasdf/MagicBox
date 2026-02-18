#include "process.h"
#include "thread.h"
#include "memory.h"
#include "stdint.h"
#include "debug.h"
#include "tss.h"
#include "console.h"
#include "string.h"
#include "global.h"
#include "interrupt.h"
#include "dlist.h"
#include "print.h"
#include "thread.h"
#include "ide.h"
#include "exec.h"
#include "vma.h"

extern void intr_exit(void);

// 释放页表所指向的数据块
void release_pg_block(struct task_struct* task){
	enum intr_status _old = intr_disable();
	uint32_t* pgdir_vaddr = task->pgdir;
	uint16_t pde_idx = 0;
	uint32_t pde = 0;
	uint32_t* v_pde_ptr = NULL;

	uint16_t pte_idx=0;
	uint32_t pte = 0;
	uint32_t* v_pte_ptr = NULL;

	uint32_t* first_pte_vaddr_in_pde = NULL;
	uint32_t pg_phy_addr = 0;

	while(pde_idx<USER_PDE_NR){
		v_pde_ptr = pgdir_vaddr+pde_idx;
		pde = *v_pde_ptr;
		if(pde&PG_P_1){
			first_pte_vaddr_in_pde = pte_ptr(pde_idx*0x400000);
			pte_idx = 0;
			while (pte_idx<USER_PTE_NR){
				v_pte_ptr = first_pte_vaddr_in_pde+pte_idx;
				pte = *v_pte_ptr;
				if(pte&0xfffff000){
					pg_phy_addr = pte&0xfffff000;
					pfree(pg_phy_addr); // 释放页表项指向的数据页
					*v_pte_ptr = 0; // 抹除映射
				}
				pte_idx++;
			}
			// 不要在 exit 中释放页表本身，因为页表本身记载着一些内核地址块的信息
			// 如果我们在 exit 或者 wait 后面突然要访问内核状态下的一些数据
			// 提前将它们释放会会导致页错误
			// pg_phy_addr = pde & 0xfffff000;
			// pfree(pg_phy_addr);
		}
		pde_idx++;
	}
	intr_set_status(_old);
}

// 释放页表本身，不释放页表指向的块
void release_pg_table(struct task_struct* task){
	enum intr_status _old = intr_disable();
	uint32_t* pgdir = task->pgdir;
	int i = 0;
	for (i = 0; i < USER_PDE_NR; i++) {
		if (pgdir[i] & PG_P_1) {
			uint32_t pt_phy_addr = pgdir[i] & 0xfffff000;
			// 释放二级页表本身
			pfree(pt_phy_addr); 
			// 将相应的项置为0，防止误访问
			pgdir[i] = 0; // 抹除映射
		}
	}
	intr_set_status(_old);
}

void release_pg_dir(struct task_struct* task){
	enum intr_status _old = intr_disable();
	// 释放一级页表（页目录）本身
	uint32_t pgdir_phy = addr_v2p((uint32_t)task->pgdir);
	pfree(pgdir_phy);
	task->pgdir = NULL; // 抹除映射
	intr_set_status(_old);
}

void user_vaddr_space_clear(struct task_struct* cur) {
    
	if (cur->pgdir == NULL) return; // 如果页目录都没了，直接返回
    release_pg_block(cur);
    release_pg_table(cur);

    // 重置用户虚拟地址位图，让 execv 后的进程从干净的状态开始
    if (cur->userprog_vaddr.vaddr_bitmap.bits != NULL) {
        memset(cur->userprog_vaddr.vaddr_bitmap.bits, 0, cur->userprog_vaddr.vaddr_bitmap.btmp_bytes_len);
    }

    // 刷新 TLB，确保旧映射彻底消失
    page_dir_activate(cur);
}

// create user proc initial context
void start_process(void* filename_){
	void* function = filename_;
	struct task_struct* cur = get_running_task_struct();
	// at first, self_kstack points to the top of the thread_stack(the lowest addr),which is the base of the kstack
	// self_kstack+=thread_stack makes it points to the top of the intr_stack
	// so we can set intr_stack in the next step
	cur->self_kstack += sizeof(struct thread_stack);
	struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;

	memset(proc_stack, 0, sizeof(struct intr_stack));

	proc_stack->gs = 0; // user mode won't use graphic segment, so set it 0
	proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA;
	proc_stack->eip = function; // The addr of the user process that is to be executed
	proc_stack->cs = SELECTOR_U_CODE;
	proc_stack->eflags = (EFLAGS_IOPL_0|EFLAGS_MBS|EFLAGS_IF_1);
	// 为用户栈分配物理地址，我们在此只分配了一个页的大小给用户栈
	proc_stack->esp = (void*)((uint32_t)mapping_v2p(PF_USER,USER_STACK3_VADDR)+PG_SIZE);
	proc_stack->ss = SELECTOR_U_STACK;
	asm volatile ("movl %0,%%esp;jmp intr_exit"::"g"(proc_stack):"memory");
}


void page_dir_activate(struct task_struct* pthread){
	uint32_t pagedir_phy_addr = 0x100000;
	// if [pthread] is a user proc
	if(pthread->pgdir!=NULL){
		// vaddr convert into paddr
		pagedir_phy_addr = addr_v2p((uint32_t)pthread->pgdir);
	}
	// update PDTR, activate the new PDT
	asm volatile ("movl %0,%%cr3"::"r"(pagedir_phy_addr):"memory");
}

void process_activate(struct task_struct* pthread){
	ASSERT(pthread!=NULL);
	page_dir_activate(pthread);
	// the priority of the kernel thread is 0
	// CPU won't get esp0 from tss when interrupt occur
	// so we don't need to update esp0
	if(pthread->pgdir){
		// if it is user proc, then update esp0
		update_tss_esp(pthread);
	}
}

uint32_t* create_page_dir(void){
	// PDT cannot be accessed by user, so allocate kernel space for it
	uint32_t* page_dir_vaddr = get_kernel_pages(1);
	
	if(page_dir_vaddr==NULL){
		console_put_str("create_page_dir: get_kernel_pages failed! ");
		return NULL;
	}
	// page_dir_vaddr + 0x300*4 is the 768th item in kernel PDT
	// means 0xc0000000
	memcpy((uint32_t*)((uint32_t)page_dir_vaddr+0x300*4),
		(uint32_t*)(0xfffff000+0x300*4),
		1024
	);
	uint32_t new_page_dir_phy_addr = addr_v2p((uint32_t)page_dir_vaddr);
	page_dir_vaddr[1023] = new_page_dir_phy_addr|PG_US_U|PG_RW_W|PG_P_1;
	return page_dir_vaddr;
}

void create_user_vaddr_bitmap(struct task_struct* user_prog){
	user_prog->userprog_vaddr.vaddr_start = USER_VADDR_START;
	// 除以8是因为8位1字节
	uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000-USER_VADDR_START)/PG_SIZE/8,PG_SIZE);
	user_prog->userprog_vaddr.vaddr_bitmap.bits = get_kernel_pages(bitmap_pg_cnt);
	user_prog->userprog_vaddr.vaddr_bitmap.btmp_bytes_len = (0xc0000000-USER_VADDR_START)/PG_SIZE/8;
	bitmap_init(&user_prog->userprog_vaddr.vaddr_bitmap);
}

// 主要是用来给main线程起用户进程
// 当一切准备就绪后，起用户进程都是通过 fork + execv 来起的
// 就不再使用此函数了
void process_execute(void* filename,char* name){
	// all PCB is in the kernel space
	struct task_struct* thread = get_kernel_pages(1);
	uint32_t prio = 0;

	prio = DEFAULT_PRIO;

	init_thread(thread,name,prio);
	create_user_vaddr_bitmap(thread);
	// start_process(filename) will be called by kernel_thread 
	thread_create(thread,start_process,filename);

	add_vma(thread, 0xBFFFF000, 0xC0000000, 0, NULL, PF_R | PF_W, 0);
	thread->start_stack = 0xc0000000;

	thread->pgdir = create_page_dir();
	
	block_desc_init(thread->u_block_desc);

	enum intr_status old_status = intr_disable();
	ASSERT(!dlist_find(&thread_ready_list, &thread->general_tag));
    dlist_push_back(&thread_ready_list, &thread->general_tag);
    ASSERT(!dlist_find(&thread_all_list, &thread->all_list_tag));
    dlist_push_back(&thread_all_list, &thread->all_list_tag);
	intr_set_status(old_status);
}