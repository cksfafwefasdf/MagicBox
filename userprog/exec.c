#include "stdint.h"
#include "stdbool.h"
#include "global.h"
#include "fs.h"
#include "memory.h"
#include "string.h"
#include "thread.h"
#include "exec.h"
#include "stdio.h"
#include "syscall.h"
#include "stdio-kernel.h"
#include "debug.h"
#include "file.h"

extern void intr_exit;

static bool segment_load(int32_t fd,uint32_t offset,uint32_t filesz,uint32_t vaddr){
	
	uint32_t vaddr_first_page = vaddr & 0xfffff000;

	uint32_t size_in_first_page = PG_SIZE - (vaddr& 0x00000fff);
	uint32_t occupy_pages = 0;

	if(filesz>size_in_first_page){
		uint32_t left_size = filesz - size_in_first_page;
		occupy_pages = DIV_ROUND_UP(left_size,PG_SIZE)+1;
	}else{
		occupy_pages = 1;
	}

	uint32_t page_idx = 0;
	uint32_t vaddr_page = vaddr_first_page;

	// printk("segment_load:::before res: %d\n",res);

	while(page_idx<occupy_pages){
		uint32_t* pde = pde_ptr(vaddr_page);
		uint32_t* pte = pte_ptr(vaddr_page);
		if(!(*pde&0x00000001)|| !(*pte & 0x00000001)){
			if(mapping_v2p(PF_USER,vaddr_page)==NULL){
				return false;
			}
		}
		vaddr_page+=PG_SIZE;
		page_idx++;
	}

	// res = bitmap_bit_check(&cur->userprog_vaddr,bit_idx);
	// printk("segment_load:::after res: %d\n",res);
	// while(1);

	// printk("prog_size: %d\n",prog_size);
	// while(1);
	sys_lseek(fd,offset,SEEK_SET);
	sys_read(fd,(void*)vaddr,filesz);
	
	return true;
}

static int32_t load(const char* pathname){
	int32_t ret = -1;
	struct Elf32_Ehdr elf_header;
	struct Elf32_Phdr prog_header;
	memset(&elf_header,0,sizeof(struct Elf32_Ehdr));

	int32_t fd = sys_open(pathname,O_RDONLY);
	if(fd == -1){
		return -1;
	}
	
	
	if(sys_read(fd,&elf_header,sizeof(struct Elf32_Ehdr))!= sizeof(struct Elf32_Ehdr)){
		ret = -1;
		goto done;
	}

	if(memcmp(elf_header.e_ident,"\177ELF\1\1\1",7)\
	|| elf_header.e_type!= 2\
	|| elf_header.e_machine!=3\
	|| elf_header.e_version!=1\
	|| elf_header.e_phnum>1024\
	|| elf_header.e_phentsize!=sizeof(struct Elf32_Phdr)){
		return -1;
		goto done;
	}

	Elf32_Off prog_header_offset = elf_header.e_phoff;
	Elf32_Half prog_header_size = elf_header.e_phentsize;

	int32_t global_fd = fd_local2global(fd);
	ASSERT(global_fd!=-1);

	uint32_t prog_idx = 0;
    uint32_t max_vaddr = 0; // 用于追踪最高虚拟地址边界
	// execv 的进程通常直接就是我们现在正在运行的程序所在的进程了
    struct task_struct* cur = get_running_task_struct();

	cur->start_code = 0;
    cur->end_code = 0;
    cur->start_data = 0;
    cur->end_data = 0;
	
	while(prog_idx<elf_header.e_phnum){
		
		memset(&prog_header,0,prog_header_size);
		
		sys_lseek(fd,prog_header_offset,SEEK_SET);
		// printf("1\n1\n1\n");
		if(sys_read(fd,&prog_header,prog_header_size)!=prog_header_size){
			ret = -1;
			goto done;
		}
		// printf("prog_header.p_type:%x\n",prog_header.p_type);
		// 所有的代码段和数据段都必须是可加载段（PT_LOAD）
		if(PT_LOAD == prog_header.p_type){
			// 加载段
			if(!segment_load(fd,prog_header.p_offset,prog_header.p_filesz,prog_header.p_vaddr)){
				ret = -1;
				goto done;
			}

			// 在写汇编或 C 代码时，我们可以定义无数个 .section .data1、.section .data2
			// 甚至在代码中间穿插数据。但当我们把它们交给链接器（Linker，如 ld）时，魔法就发生了
			// 链接器会根据“访问权限”和“加载属性”把所有的 Section 重新排列。
			// 它会把所有具有“只读+执行”权限的段（如 .text、.rodata）排在一起，把所有具有“可读写”权限的段（如 .data、.bss）排在一起。
			// Section（节）：是给链接器看的（如 .text, .data, .bss）。
			// Segment（段/程序头）：是给操作系统加载器（也就是 load 函数）看的。
			// 一个 Segment 通常包含一个或多个 Section。 
			// 链接器会生成一个巨大的 PT_LOAD Segment，里面塞满了所有的 .text；
			// 然后再生成另一个 PT_LOAD Segment，里面塞满了所有的 .data 和 .bss。
			// 链接器生成的 Segment 是按照地址从小到大排列的。通常代码段在低地址，数据段在高地址。
			// 无论中间有多少个琐碎的段，对于实现 brk 来说，我们只需要知道整个程序占用的虚拟空间的最顶端在哪里。从那个顶端往上的空间，就是自由的堆。
			// 通常来说每个程序只会有一个代码段和数据段，但若不幸，我们有某个有多个数据段的程序，那么
			// end_code 最终指向的是最后一个代码类段的结束。
			// end_data 最终指向的是最后一个数据类段的结束（即 BSS 结束处）。
			// brk 从这个 end_data 开始起跑。因此依然正确
			uint32_t vaddr_end = prog_header.p_vaddr + prog_header.p_memsz;
			if (!(prog_header.p_flags & PF_W)) { // PF_W (写) = 2,
				// 如果没有写权限，那么就是代码段
				// 实际上这不太严谨，只读数据段 .rodata 也没有写权限
				// 在此处，我们一样将其看作是代码段的一部分，这和linux的做法一样
				// 通常来说，只读数据段(.rodata .text)都是连在一起的
				// 因此我们将第一个只读数据段的起始地址作为 start_code
				// 最后一个只读数据段的结束地址作为 end_code
				// 但是，通常来说，第一个只读段都是 elf 头所在的位置，并不是 .text
				// 但是这没啥关系，因为这和linux的表现一致
				// 只有第一次遇到只读段时，才记录 start_code
				if (cur->start_code == 0) {
					cur->start_code = prog_header.p_vaddr;
				}
				cur->end_code = vaddr_end;
			} else { // 有写权限 -> 数据段
				cur->start_data = prog_header.p_vaddr;
				cur->end_data = vaddr_end; // 数据/BSS 结束的地方
			}

			if (vaddr_end > max_vaddr) {
				max_vaddr = vaddr_end;
			}
			
		}

		prog_header_offset+=elf_header.e_phentsize;
		prog_idx++;
	}

	cur->end_data = max_vaddr; // 程序的终点即为堆的起点
    cur->brk = max_vaddr; // 初始时堆顶等于堆起点
	// 程序的栈起始位置
	// 由于我们划分了高1GB的虚拟地址给内核空间，而栈是从高向低生长的
	// 因此设置高1GB的最开始的地址 0xc0000000 作为栈底是合适的，可用空间很大
    cur->start_stack = 0xc0000000; 

	// printk("load: start_code:%x end_data:%x start_stack:%x\n",cur->start_code,cur->end_data,cur->start_stack);

	ret = elf_header.e_entry;

done:
	sys_close(fd);
	return ret;
}

int32_t sys_execv(const char* path,const char* argv[]){
	
	uint32_t argc = 0;
	
	while(argv!=NULL&&argv[argc]){
		argc++;
	}
	
	int32_t entry_point = load(path);
	

	if(entry_point==-1){
		return -1;
	}

	struct task_struct* cur = get_running_task_struct();
	memcpy(cur->name,path,TASK_NAME_LEN);
	cur->name[TASK_NAME_LEN-1] = 0;

	struct intr_stack* intr_0_stack = (struct intr_stack*) ((uint32_t)cur+PG_SIZE-sizeof(struct intr_stack));

	intr_0_stack->ebx = (int32_t)argv;
	intr_0_stack->ecx = argc;
	intr_0_stack->eip = (void*) entry_point;
	intr_0_stack->esp = (void*) 0xc0000000;

	int i=0;
	// fork 出的子进程会全权继承父进程的信号处理操作
	// execv 出来的进程当父进程中的处理函数为SIG_IGN时，仍然保留
	// 否则将其全部置为 SIG_DFL
	// SIG_IGN 必须保留，这允许 Shell 实现 nohup 或后台静默运行。
	for(i=0;i<SIG_NR;i++){
		if (cur->sigactions[i].sa_handler != SIG_IGN) {
			cur->sigactions[i].sa_handler = SIG_DFL;
			cur->sigactions[i].sa_mask = 0;
			cur->sigactions[i].sa_flags = 0;
			cur->sigactions[i].sa_restorer = NULL;
    	}
	}

	
	asm volatile ("movl %0,%%esp; jmp intr_exit"::"g"(intr_0_stack):"memory");
	return 0;
}