#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "dlist.h"
#include "interrupt.h"
#include "debug.h"
#include "print.h"
#include "process.h"
#include "sync.h"
#include "main.h"
#include "stdio.h"
#include "fs.h"
#include "file.h"
#include "dlist.h"

// max number of pid is 128*8=1024
// use bitmap to check if the pid is in used
uint8_t pid_bimap_bits[MAX_PID_NUM_IN_BYTE] = {0};

struct pid_pool{
	struct bitmap pid_bitmap;
	uint32_t pid_start;
	struct lock pid_lock;
}pid_pool;


struct task_struct* main_thread; // main thread PCB
struct dlist thread_ready_list;
struct dlist thread_all_list; // queue of all tasks
static struct dlist_elem* thread_tag; 

static void pid_pool_init(void);

// struct lock pid_allocate_lock;

struct task_struct* idle_thread;

static pid_t allocate_pid(void);

extern void switch_to(struct task_struct* cur,struct task_struct* next);

static void idle(void *arg);

// get the PCB pointer of the currently running thread
struct task_struct* get_running_task_struct(){
	// esp points to k_stack now, and both k_stack and PCB are in the same page
	// therefore,the high 20 bits of esp represent the start address of the page
	uint32_t esp;
	asm("mov %%esp,%0":"=g"(esp));
	// get the start address of the PCB, which is the beginning of the page
	return (struct task_struct*)(esp&0xfffff000);
}

// typedef void thread_func(void*)
static void kernel_thread(thread_func* function,void* func_arg){
	// enable interrupt to prevent timer-interrupt from being masked
	intr_enable();
	function(func_arg);
}

void init_thread(struct task_struct* pthread,char* name,int prio){
	// pthread is task_struct*
	// so *pthread is task_struct
	memset(pthread,0,sizeof(*pthread));

	pthread->pid = allocate_pid();

	// max length is 16byte
	strcpy(pthread->name,name);

	// at first, only the main thread can use the CPU
	if(pthread==main_thread){
		pthread->status = TASK_RUNNING;
	}else{
		pthread->status = TASK_READY;
	}

	pthread->self_kstack = (uint32_t*)((uint32_t)pthread+PG_SIZE);
	pthread->priority = prio;
	pthread->ticks = prio;
	pthread->elapsed_ticks = 0;
	pthread->pgdir = NULL;

	// reserve for stdin, stdout, stderr
	pthread->fd_table[0] = 0; // stdin
	pthread->fd_table[1] = 1; // stdout
	pthread->fd_table[2] = 2; // stderr
	// the others set as -1
	uint8_t fd_idx = 3;
	while(fd_idx<MAX_FILES_OPEN_PER_PROC){
		pthread->fd_table[fd_idx] = -1;
		fd_idx++;
	}

	pthread->cwd_inode_nr = 0;
	pthread->parent_pid = -1;

	pthread->stack_magic = 0x20030607;
}

void thread_create(struct task_struct* pthread,thread_func* function,void* func_arg){
	//reserve space for intr_stack
	pthread->self_kstack -= sizeof(struct intr_stack);
	//reserve space for thread_stack
	pthread->self_kstack -= sizeof(struct thread_stack);

	struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
	kthread_stack->eip = kernel_thread;
	kthread_stack->function = function;
	kthread_stack->func_arg = func_arg;
	kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0;
}

// All PCBs reside in the kernel space, whether they belong to user processes or kernel processes
struct task_struct* thread_start(char* name,int prio,thread_func function,void* func_arg){
	struct task_struct* thread = get_kernel_pages(1);
	init_thread(thread,name,prio);
	thread_create(thread,function,func_arg);
	// ensure this thread is not in the queue before we createing
	ASSERT(!dlist_find(&thread_ready_list,&thread->general_tag));
	dlist_push_back(&thread_ready_list,&thread->general_tag);
	// as same as above
	ASSERT(!dlist_find(&thread_all_list,&thread->all_list_tag));
	dlist_push_back(&thread_all_list,&thread->all_list_tag);

	/* asm volatile ("movl %0,%%esp; \
					pop %%ebp;pop %%ebx;pop %%edi;pop %%esi; \
					ret"::"g"(thread->self_kstack):"memory"); */
	return thread;
}

// make kernel main thread
// the space for the kernel main thread is reserved in loader.s
static void make_main_thread(void){
	// loader.s: mov esp,0xc009f000 
	// 0xc009e000 ~ 0xc009efff is reserved for the kernel main thread
	// therefore, we don't need to allocate an additional page for the kernel main thread
	main_thread = get_running_task_struct();
	init_thread(main_thread,"main",31);

	ASSERT(!dlist_find(&thread_all_list,&main_thread->all_list_tag));
	dlist_push_back(&thread_all_list,&main_thread->all_list_tag);
}

void schedule(){
    // scheduling process must be conducted in INTR_OFF 
    ASSERT(intr_get_status()==INTR_OFF);

	struct task_struct* cur = get_running_task_struct();
	if(cur->status==TASK_RUNNING){
		// if time slice is depleted
		ASSERT(!dlist_find(&thread_ready_list,&cur->general_tag));
		dlist_push_back(&thread_ready_list,&cur->general_tag);
		// restore ticks
		cur->ticks = cur->priority;
		cur->status = TASK_READY;
	}else{
		// block task
	}

	//ASSERT(!dlist_empty(&thread_ready_list));

	// if no threads are ready to run, then run the idle thread
	if(dlist_empty(&thread_ready_list)){
		thread_unblock(idle_thread);
	}


	thread_tag = NULL; // clear tag
	thread_tag = dlist_pop_front(&thread_ready_list);
	struct task_struct* next = elem2entry(struct task_struct,thread_tag);
	next->status = TASK_RUNNING;
	process_activate(next);
	//put_char('\n');put_str(cur->name);put_str(" switch to ");put_str(next->name);put_char('\n');
	switch_to(cur,next);
}

// void thread_environment_init(void){
// 	put_str("thread_environment_init start\n");
// 	dlist_init(&thread_ready_list);
// 	dlist_init(&thread_all_list);
// 	lock_init(&pid_allocate_lock);
// 	make_main_thread();

// 	idle_thread = thread_start("idle",10,idle,NULL);

// 	put_str("thread_environment_init done\n");
// }

void thread_environment_init(void){
	put_str("thread_environment_init start\n");
	
	dlist_init(&thread_ready_list);
	dlist_init(&thread_all_list);
	// lock_init(&pid_allocate_lock);
	pid_pool_init();

	process_execute(init,"init");
	
	make_main_thread();

	idle_thread = thread_start("idle",10,idle,NULL);

	put_str("thread_environment_init done\n");
}

// block thread itself 
// and set status as [stat]
void thread_block(enum task_status stat){
	ASSERT((stat==TASK_BLOCKED)||(stat==TASK_WAITING)||(stat==TASK_HANGING));
	enum intr_status old_stat = intr_disable();
	struct task_struct* cur_thread = get_running_task_struct();
	cur_thread->status = stat;
	schedule();
	intr_set_status(old_stat);
}

void thread_unblock(struct task_struct* pthread){
	enum intr_status old_stat = intr_disable();
	ASSERT((pthread->status==TASK_BLOCKED)||(pthread->status==TASK_WAITING)||(pthread->status==TASK_HANGING));
	if(pthread->status!=TASK_READY){
		ASSERT(!dlist_find(&thread_ready_list,&pthread->general_tag));
		if(dlist_find(&thread_ready_list,&pthread->general_tag)){
			PANIC("thread_unblock: blocked thread in ready list\n");
		}
		dlist_push_front(&thread_ready_list,&pthread->general_tag);
		pthread->status = TASK_READY;
	}
	intr_set_status(old_stat);
}

static pid_t allocate_pid(void){
	lock_acquire(&pid_pool.pid_lock);
	int32_t bit_idx = bitmap_scan(&pid_pool.pid_bitmap,1);
	bitmap_set(&pid_pool.pid_bitmap,bit_idx,1);
	lock_release(&pid_pool.pid_lock);
	return bit_idx+pid_pool.pid_start;
}

// when thread_yield is called, the status of the running thread becomes TASK_READY 
void thread_yield(void){
	struct task_struct* cur = get_running_task_struct();
	enum intr_status old_status = intr_disable();
	ASSERT(!dlist_find(&thread_ready_list,&cur->general_tag));
	dlist_push_back(&thread_ready_list,&cur->general_tag);
	cur->status = TASK_READY;
	schedule();
	intr_set_status(old_status);
}

// run the idle thread when system is not busy 
static void idle(void *arg UNUSED){
	while (1){
		thread_block(TASK_BLOCKED);
		asm volatile("sti;hlt":::"memory");
	}
}

pid_t fork_pid(){
	return allocate_pid();
}

static void pad_print(char* buf,int32_t buf_len,void* ptr,char format){
	memset(buf,0,buf_len);
	uint8_t out_pad_0idx = 0;
	switch (format){
		case 's':
			out_pad_0idx = sprintf(buf,"%s",ptr);
			break;
		case 'd':
			out_pad_0idx = sprintf(buf,"%d",*((int16_t*)ptr));
		case 'x':
			out_pad_0idx = sprintf(buf,"%x",*((uint32_t*)ptr));
	}

	while(out_pad_0idx<buf_len){
		buf[out_pad_0idx] = ' ';
		out_pad_0idx++;
	}	
	sys_write(stdout_no,buf,buf_len-1);
}


static bool elem2thread_info(struct dlist_elem* pelem,int arg UNUSED){
	struct task_struct* pthread = member_to_entry(struct task_struct,all_list_tag,pelem);

	char out_pad[16] = {0};

	pad_print(out_pad,16,&pthread->pid,'d');

	if(pthread->parent_pid == -1){
		pad_print(out_pad,16,"NULL",'s');
	}else{
		pad_print(out_pad,16,&pthread->parent_pid,'d');
	}

	switch (pthread->status){
		case 0:
			pad_print(out_pad,16,"RUNNING",'s');
			break;
		case 1:
			pad_print(out_pad,16,"READY",'s');
			break;
		case 2:
			pad_print(out_pad,16,"BLOCKED",'s');
			break;
		case 3:
			pad_print(out_pad,16,"WATTING",'s');
			break;
		case 4:
			pad_print(out_pad,16,"HANGING",'s');
			break;
		case 5:
			pad_print(out_pad,16,"DIED",'s');
			break;
	}

	pad_print(out_pad,16,&pthread->elapsed_ticks,'x');

	memset(out_pad,0,16);
	ASSERT(strlen(pthread->name)<17);
	memcpy(out_pad,pthread->name,strlen(pthread->name));
	strcat(out_pad,"\n");
	sys_write(stdout_no,out_pad,strlen(out_pad));
	return false;
}

void sys_ps(void){
	char* ps_title = "PID\t\t\tPPID\t\t   STAT\t\t   TICKS\t\t  COMMAND\t\t\n";
	sys_write(stdout_no,ps_title,strlen(ps_title));
	dlist_traversal(&thread_all_list,elem2thread_info,0);
}

static void pid_pool_init(void){
	pid_pool.pid_start = 1;
	pid_pool.pid_bitmap.bits = pid_bimap_bits;
	pid_pool.pid_bitmap.btmp_bytes_len = MAX_PID_NUM_IN_BYTE;
	bitmap_init(&pid_pool.pid_bitmap);
	lock_init(&pid_pool.pid_lock);
}

void release_pid(pid_t pid){
	lock_acquire(&pid_pool.pid_lock);
	int32_t bit_idx = pid-pid_pool.pid_start;
	bitmap_set(&pid_pool.pid_bitmap,bit_idx,0);
	lock_release(&pid_pool.pid_lock);
}


void thread_exit(struct task_struct* thread_over,bool need_schedule){
	intr_disable();
	thread_over->status = TASK_DIED;

	if(dlist_find(&thread_ready_list,&thread_over->general_tag)){
		dlist_remove(&thread_over->general_tag);
	}

	if(thread_over->pgdir){
		mfree_page(PF_KERNEL,thread_over->pgdir,1);
	}

	dlist_remove(&thread_over->all_list_tag);

	if(thread_over!=main_thread){
		mfree_page(PF_KERNEL,thread_over,1);
	}

	release_pid(thread_over->pid);

	if(need_schedule){
		schedule();
		PANIC("thread_exit: should not be here!\n");
	}
}


static bool pid_check(struct dlist_elem* pelem,int32_t pid){
	struct task_struct* pthread = member_to_entry(struct task_struct,all_list_tag,pelem);
	if(pthread->pid==pid){
		return true;
	}
	return false;
}

struct task_struct* pid2thread(int32_t pid){
	struct dlist_elem* pelem = dlist_traversal(&thread_all_list,pid_check,pid);
	if(pelem==NULL){
		return NULL;
	}
	struct task_struct* thread = member_to_entry(struct task_struct,all_list_tag,pelem);
	return thread;
}


