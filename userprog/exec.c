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
#include "process.h"
#include "wait_exit.h"
#include "vma.h"

extern void intr_exit;

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

	struct m_inode* file_inode = file_table[global_fd].fd_inode; // 获取 inode，以便于填充vma

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
			// if(!segment_load(fd,prog_header.p_offset,prog_header.p_filesz,prog_header.p_vaddr)){
			// 	ret = -1;
			// 	goto done;
			// }

			add_vma(cur, 
                    prog_header.p_vaddr, // start
                    prog_header.p_vaddr + prog_header.p_memsz, // end (含BSS)
                    prog_header.p_offset, // 文件偏移
                    file_inode, // 关联 inode
                    prog_header.p_flags, // 权限
					prog_header.p_filesz); // 大小

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

	// 为堆 (Heap) 签署一个初始合同
    // 初始大小为 0，但 VMA 必须存在，后续 sbrk 会通过修改 vma_end 来扩容
	// 堆栈是非文件区域，filesz要设置为0
    add_vma(cur, max_vaddr, max_vaddr, 0, NULL, PF_R | PF_W,0);

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

// sys_execv 必须要有参数！至少要有第一个参数，即文件名！
// argv[0] 和 path 不总是一样，因为 argv[0] 可能是 ls ，而 path 是 /bin/ls
// 并且 argv 必须以 NULL 结尾
int32_t sys_execv(const char* path, const char* argv[]) {

	if(argv==NULL){
		printk("sys_execv dose not allow argv to be NULL !\n");
		return -1;
	}

    uint32_t argc = 0;
    struct task_struct* cur = get_running_task_struct();

    // 准备内核中转页，用于备份参数
    // 必须在 user_vaddr_space_clear 之前完成，因为我们需要读取用户态的 path 和 argv
    char* k_arg_page = get_kernel_pages(1); 
    
	if (k_arg_page == NULL){
		goto fail1;
	}

	char* path_bk =  kmalloc(strlen(path) + 1); // +1 是加上一个 '\0'
	
	if (path_bk == NULL){
		goto fail2;
	}

	memset(k_arg_page,0,PG_SIZE);
	strcpy(path_bk,path); // 备份路径

    char* k_ptr = k_arg_page;
    uint32_t arg_offsets[MAX_ARG_NR] = {0};
    uint32_t arg_lens[MAX_ARG_NR] = {0};

    // 备份参数，第一个参数是文件名
	int i = 0; 
	// 由于我们是以 argv[i] != NULL 作为边界条件的，因此 argv 必须以 NULL 结尾
	while (argv[i] != NULL && argc < MAX_ARG_NR) {
		uint32_t cur_len = strlen(argv[i]) + 1;
		
		// 检查内核页是否溢出
		if ((k_ptr - k_arg_page) + cur_len > PG_SIZE) goto fail3;

		arg_offsets[argc] = (uint32_t)(k_ptr - k_arg_page);
		arg_lens[argc] = cur_len;
		strcpy(k_ptr, argv[i]);
		
		k_ptr += cur_len;
		argc++;
		i++;
	}

	// 加载新程序时，先清空旧的 vma 链表
	clear_vma_list(cur);

    // 清理旧的用户空间映射 (0 ~ 3GB)
    // 此时用户栈被销毁，用户态指针 path 和 argv 彻底失效
    user_vaddr_space_clear(cur);

	// 预留 8MB 空间给栈 (0xc0000000 - 8MB 到 0xc0000000)
    // 这样接下来的参数拷贝触发缺页时，find_vma 就能找到它
	// 堆栈是非文件区域，filesz要设为0

	// 目前，我们的malloc和free直接绕过了堆的vma进行操作
	// 因此目前堆的vma只是一个占位，等待后期完善
    add_vma(cur, 0xc0000000 - 0x800000, 0xc0000000, 0, NULL, PF_R | PF_W,0);

	// printk("argv[0]:%s \n",k_arg_page + arg_offsets[0]);
    // 加载新程序，传入内核空间的 path 副本
    // 此时读取 path 访问的是内核地址 (0xC0000000以上)，绝对不会触发用户空间缺页

    int32_t entry_point = load(path_bk); 

    if (entry_point == -1) {
		printk("execv: load failed! killing process %d\n", cur->pid);
		kfree(path_bk);
		mfree_page(PF_KERNEL, k_arg_page, 1);
		
		// 必须走 sys_exit。
		// 既然已经 user_vaddr_space_clear 了，这个进程已经无法生存，
		// 必须通过正规渠道“宣布死亡”，让父进程收尸。
		sys_exit(-1); 
	}

    // 更新进程名
    memcpy(cur->name, path_bk, TASK_NAME_LEN);
    cur->name[TASK_NAME_LEN - 1] = 0;
	

    // 准备新程序的用户栈
    // 栈底设为 0xc0000000
    uint32_t user_stack_top = 0xc0000000;
    uint32_t new_argv_pointers[MAX_ARG_NR] = {0};

    // 将内核备份的参数拷贝到新栈顶
    // 第一次 memcpy 写入 0xbffffxxx 时，CPU 会自动触发 swap_page 进行扩容
    for (i = (int)argc - 1; i >= 0; i--) {
        uint32_t cur_len = arg_lens[i];
        user_stack_top -= cur_len;
        
        // 这一步会通过缺页中断自动建立物理映射
        memcpy((void*)user_stack_top, k_arg_page + arg_offsets[i], cur_len);
        new_argv_pointers[i] = user_stack_top; 
    }
	

    // 拷贝指针数组到栈顶 (argv 列表) 
    uint32_t argv_table_size = sizeof(char*) * (argc + 1);
    user_stack_top -= argv_table_size;
    
    char** user_argv_list = (char**)user_stack_top;
    // 如果跨页了也会触发 swap_page，但可以自动修复，不要紧
    user_argv_list[argc] = NULL; // 最后一个参数置为 NULL
    for (i = 0; i < argc; i++) {
        user_argv_list[i] = (char*)new_argv_pointers[i];
    }

    // 准备进入用户态的中断栈上下文
    struct intr_stack* intr_0_stack = (struct intr_stack*)((uint32_t)cur + PG_SIZE - sizeof(struct intr_stack));

    intr_0_stack->ebx = (int32_t)user_argv_list; // 存入新程序的 argv 指针
    intr_0_stack->ecx = (int32_t)argc;           // 存入 argc
    intr_0_stack->eip = (void*)entry_point;      // 新程序入口
    intr_0_stack->esp = (void*)user_stack_top;   // 新程序栈顶

    // 信号处理重置 (按照posix标准，要保留父进程的 SIG_IGN，其余的全部置为默认)
    for (i=0 ; i < SIG_NR; i++) {
        if (cur->sigactions[i].sa_handler != SIG_IGN) {
            cur->sigactions[i].sa_handler = SIG_DFL;
            cur->sigactions[i].sa_mask = 0;
            cur->sigactions[i].sa_flags = 0;
            cur->sigactions[i].sa_restorer = NULL;
        }
    }

    // 释放中转内存
    mfree_page(PF_KERNEL, k_arg_page, 1);
	kfree(path_bk);

    // 切换栈并跳转到 intr_exit 执行 iret 进入用户态
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (intr_0_stack) : "memory");

    return 0; // 不会执行到这里

fail3:
	kfree(path_bk);
fail2:
    mfree_page(PF_KERNEL, k_arg_page, 1);
fail1:
	return -1;
}