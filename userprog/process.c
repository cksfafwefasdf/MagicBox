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

extern void intr_exit(void);

// create user proc initial context
void start_process(void* filename_){
	void* function = filename_;
	struct task_struct* cur = get_running_task_struct();
	// at first, self_kstack points to the top of the thread_stack(the lowest addr),which is the base of the kstack
	// self_kstack+=thread_stack makes it points to the top of the intr_stack
	// so we can set intr_stack in the next step
	cur->self_kstack += sizeof(struct thread_stack);
	struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;
	proc_stack->edi = proc_stack->esi = proc_stack->ebp = proc_stack->esp_dummy=0;

	proc_stack->ebx = proc_stack->edx=proc_stack->ecx=proc_stack->eax=0;
	proc_stack->gs = 0; // user mode won't use graphic segment, so set it 0
	proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA;
	proc_stack->eip = function; // The addr of the user process that is to be executed
	proc_stack->cs = SELECTOR_U_CODE;
	proc_stack->eflags = (EFLAGS_IOPL_0|EFLAGS_MBS|EFLAGS_IF_1);
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

void process_execute(void* filename,char* name){
	// all PCB is in the kernel space
	struct task_struct* thread = get_kernel_pages(1);
	uint32_t prio = 0;

	prio = DEFAULT_PRIO;

	init_thread(thread,name,prio);
	create_user_vaddr_bitmap(thread);
	// start_process(filename) will be called by kernel_thread 
	thread_create(thread,start_process,filename);
	thread->pgdir = create_page_dir();
	
	block_desc_init(thread->u_block_desc);

	enum intr_status old_status = intr_disable();
	ASSERT(!dlist_find(&thread_ready_list, &thread->general_tag));
    dlist_push_back(&thread_ready_list, &thread->general_tag);
    ASSERT(!dlist_find(&thread_all_list, &thread->all_list_tag));
    dlist_push_back(&thread_all_list, &thread->all_list_tag);
	intr_set_status(old_status);
}

