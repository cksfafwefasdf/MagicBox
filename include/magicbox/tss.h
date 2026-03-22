#ifndef __INCLUDE_MAGICBOX_TSS_H
#define __INCLUDE_MAGICBOX_TSS_H

struct task_struct;


struct tss{
	uint32_t backlink; // ptr for last task TSS
	uint32_t* esp0;
	uint32_t ss0;
	uint32_t* esp1;
	uint32_t ss1;
	uint32_t* esp2;
	uint32_t ss2;
	uint32_t cr3;
	uint32_t (*eip) (void);
	uint32_t eflags;
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t es;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
	uint32_t ldt_desc;
	uint16_t trace;
	uint16_t io_base;
	// 借用 TSS 空间存放当前任务指针
	// 以便给 get_running_task_struct 用
    // 这个值在 tss_init 中被初始化，然后在每次process_activate中被更新
    // 我们借用tss来存储cur_task的原因很简单
    // tss 在特权级切换时会被使用到
    // 但是在没进行特权级切换时，它不会被激活，此时我们不会执行update_tss_esp操作
    // 因此我们需要将 cur_task 的更新放到update_tss_esp外，最后选定放到 process_activate 中
    // 用 tss 存当前进程还有一个原因，每一个cpu都只会有一个tss，这是在硬件层面上的规定
    // 因此借用tss的结构体来存cur_task天然对多核系统就很友好吗，这样以后写多核相关的逻辑也会方便很多
    // 当我们更新tss时，只会把上面那些数据交给cpu，这个cur_task cpu读不到，因为这是个硬件提交的操作
    // 硬件读不到这里
	struct task_struct* cur_task; 
};

extern struct tss tss;

extern void update_tss_esp(struct task_struct* pthread);
extern void tss_init(void);

#endif