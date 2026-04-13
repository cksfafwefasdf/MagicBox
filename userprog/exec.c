#include <stdint.h>
#include <stdbool.h>
#include <global.h>
#include <fs.h>
#include <memory.h>
#include <string.h>
#include <thread.h>
#include <exec.h>
#include <stdio.h>
#include <syscall.h>
#include <stdio-kernel.h>
#include <debug.h>
#include <file_table.h>
#include <process.h>
#include <wait_exit.h>
#include <vma.h>
#include <fcntl.h>

extern void intr_exit;

static int32_t load(const char* pathname){
	int32_t ret = -1;
	struct Elf32_Ehdr elf_header;
	struct Elf32_Phdr prog_header;
	memset(&elf_header,0,sizeof(struct Elf32_Ehdr));

	int32_t fd = sys_open(pathname,O_RDONLY);
	if(fd < 0){
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

	int32_t global_fd = fd_local2global(get_running_task_struct(), fd);

	struct inode* file_inode = file_table[global_fd].fd_inode; // 获取 inode，以便于填充vma

	ASSERT(global_fd!=-1);

	uint32_t prog_idx = 0;
    uint32_t max_vaddr = 0; 
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

			// VMA_READ 的flag的定义和 PF_R 的定义不一样，所以得翻译一下
			uint32_t vma_flags = 0;

			uint32_t vaddr_start = prog_header.p_vaddr;
    		uint32_t vaddr_end = vaddr_start + prog_header.p_memsz;

			// 按照elf的标准，
			// 把起始地址向下对齐起始地址 (例如 0x0804bff8 -> 0x0804b000)
			uint32_t aligned_vaddr_start = vaddr_start & 0xfffff000;
			// 把结束地址向上对齐结束地址
			uint32_t aligned_vaddr_end = (vaddr_end + 0xfff) & 0xfffff000;

			// 翻译权限 (从 ELF 转换到 VM_ 标志)
			if (prog_header.p_flags & PF_R) vma_flags |= VM_READ;
			if (prog_header.p_flags & PF_W) vma_flags |= VM_WRITE;
			if (prog_header.p_flags & PF_X) vma_flags |= VM_EXEC;

			uint32_t offset = vaddr_start - aligned_vaddr_start;
			add_vma(cur, 
                    aligned_vaddr_start, // start
                    aligned_vaddr_end, // end (含BSS)
                    prog_header.p_offset - offset, // 文件偏移
                    file_inode, // 关联 inode
                    vma_flags, // 权限
					prog_header.p_filesz + offset);// 大小

			// printk("Segment %d: vaddr 0x%x, filesz 0x%x, memsz 0x%x\n", prog_idx, prog_header.p_vaddr, prog_header.p_filesz, prog_header.p_memsz);

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
			// uint32_t vaddr_end = prog_header.p_vaddr + prog_header.p_memsz;
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

	// 找到程序最高的地址后，强行对齐到 4KB 边界
	// 否则的化，例如 .bss 结束于 0x804D010。
	// 如果不进行 PAGE_ALIGN_UP，堆（Heap）会从 0x804D010 开始。
	// 此时，.bss 段的末尾和堆的起始处物理上挤在同一个 4KB 页面内（即 0x804D000 这一页）。
	// 这意味着，这一个物理页既要承载全局变量，又要承载堆内存。
	// 此时要是处理一些缺页错误啥的，可能会将原本bss段的数据都给破坏了！
	// 为了简单，我们直接将堆设在bss段紧邻的下一个页的起始处
	max_vaddr = PAGE_ALIGN_UP(max_vaddr); // 

	// 这样堆就会从 0x0804e000 或更高的地方开始
	// 初始给他划分一页的逻辑空间，物理空间等到缺页了再去映射
	add_vma(cur, max_vaddr, max_vaddr + PG_SIZE, 0, NULL, VM_READ | VM_WRITE | VM_GROWSUP | VM_ANON, 0);
	// 程序的终点即为堆的起点
	// 其实这个数据主要是用来记录堆的起点的，因此它需要跟随向上对齐一页后的max_vaddr
	cur->end_data = max_vaddr; 
    cur->brk = max_vaddr; // 初始时堆顶等于堆起点
	// 程序的栈起始位置
	// 由于我们划分了高1GB的虚拟地址给内核空间，而栈是从高向低生长的
	// 因此设置高1GB的最开始的地址 0xc0000000 作为栈底是合适的，可用空间很大
    cur->start_stack = USER_STACK_BASE; 

	// printk("load: start_code:%x end_data:%x start_stack:%x\n",cur->start_code,cur->end_data,cur->start_stack);

	ret = elf_header.e_entry;

done:
	sys_close(fd);
	return ret;
}

// 在 Linux 的实现中，只有带环境变量的 execve ，execv 是库函数封装的
int32_t sys_execve(const char* path, const char* argv[], const char* envp[]) {
    if (argv == NULL) return -1;

    struct task_struct* cur = get_running_task_struct();
    uint32_t argc = 0;
    uint32_t envc = 0;
	
    // 准备内核中转页，用于备份参数
    // 必须在 user_vaddr_space_clear 之前完成，因为我们需要读取用户态的 path 和 argv
    char* k_arg_page = get_kernel_pages(1);
    if (!k_arg_page) return -1;
    char* path_bk = kmalloc(strlen(path) + 1); // +1 是加上一个 '\0'
    if (!path_bk) { mfree_page(PF_KERNEL, k_arg_page, 1); return -1; }
    strcpy(path_bk, path); // 备份路径

    char* k_ptr = k_arg_page;
    uint32_t arg_offsets[MAX_ARG_NR], arg_lens[MAX_ARG_NR];
    uint32_t env_offsets[MAX_ARG_NR], env_lens[MAX_ARG_NR];

    
	// 备份参数，第一个参数是文件名
	// 由于我们是以 argv[i] != NULL 作为边界条件的，因此 argv 必须以 NULL 结尾
	// 拷贝 argv 字符串
    while (argv[argc] && argc < MAX_ARG_NR) {
        arg_lens[argc] = strlen(argv[argc]) + 1;
        arg_offsets[argc] = (uint32_t)(k_ptr - k_arg_page);
        strcpy(k_ptr, argv[argc]);
        k_ptr += arg_lens[argc];
        argc++;
    }

    // 拷贝 envp 字符串 (如果 envp 为 NULL 则 envc 为 0)
    while (envp && envp[envc] && (argc + envc) < MAX_ARG_NR) {
        env_lens[envc] = strlen(envp[envc]) + 1;
        env_offsets[envc] = (uint32_t)(k_ptr - k_arg_page);
        strcpy(k_ptr, envp[envc]);
        k_ptr += env_lens[envc];
        envc++;
    }

    // 加载 ELF 文件并清理旧进程空间
	// 加载新程序时，先清空旧的 vma 链表
    clear_vma_list(cur);
	// 清理旧的用户空间映射 (0 ~ 3GB)
    // 此时用户栈被销毁，用户态指针 path 和 argv 彻底失效
    user_vaddr_space_clear(cur);

	// 遍历当前进程的所有文件描述符
    for (int i = 0; i < MAX_FILES_OPEN_PER_PROC; i++) {
        // 如果 global_fd_idx 有效，且标志位设置了 FD_CLOEXEC 
        if (cur->fd_table[i].global_fd_idx != -1 && (cur->fd_table[i].flags & FD_CLOEXEC)) {
            // 调用 sys_close。这会处理全局引用计数 f_count 和 inode 的关闭
            sys_close(i);
        }
    }
	
	// 预留 8MB 空间给栈 (0xc0000000 - 8MB 到 0xc0000000)
    // 这样接下来的参数拷贝触发缺页时，find_vma 就能找到它
	// 堆栈是非文件区域，filesz要设为0
    add_vma(cur, USER_STACK_BASE - USER_STACK_SIZE, USER_STACK_BASE, 0, NULL, VM_READ | VM_WRITE | VM_GROWSDOWN | VM_ANON, 0);

	// 加载新程序，传入内核空间的 path 副本
    // 此时读取 path 访问的是内核地址 (0xC0000000以上)，绝对不会触发用户空间缺页
    int32_t entry_point = load(path_bk);
    if (entry_point == -1) {
        kfree(path_bk); mfree_page(PF_KERNEL, k_arg_page, 1);
		// 必须走 sys_exit。
		// 既然已经 user_vaddr_space_clear 了，这个进程已经无法生存，
		// 必须通过正规渠道“宣布死亡”，让父进程收尸。
        sys_exit(-1);
    }

    // 准备用户栈 (从高向低压入)
    // 栈底设为 0xc0000000
    uint32_t user_stack_top = USER_STACK_BASE;
    uint32_t k_envp_ps[MAX_ARG_NR], k_argv_ps[MAX_ARG_NR];

    // 先压入所有字符串内容
	// 将内核备份的参数拷贝到新栈顶
    // 第一次 memcpy 写入 0xbffffxxx 时，CPU 会自动触发 swap_page 进行扩容
    for (int i = (int)envc - 1; i >= 0; i--) {
        user_stack_top -= env_lens[i];
		// 这一步会通过缺页中断自动建立物理映射
        memcpy((void*)user_stack_top, k_arg_page + env_offsets[i], env_lens[i]);
        k_envp_ps[i] = user_stack_top;
    }

    for (int i = (int)argc - 1; i >= 0; i--) {
        user_stack_top -= arg_lens[i];
        memcpy((void*)user_stack_top, k_arg_page + arg_offsets[i], arg_lens[i]);
        k_argv_ps[i] = user_stack_top;
    }

    // 压入指针数组 (必须 4 字节对齐)
    uint32_t* sp = (uint32_t*)(user_stack_top & ~3);

    // 压入辅助向量结束标志 (Auxv)  musl 启动需要用到
    sp--; *sp = 0; 

    // 压入 envp 指针数组 (以 NULL 结尾)
    sp--; *sp = 0; 
    for (int i = (int)envc - 1; i >= 0; i--) {
        sp--; *sp = k_envp_ps[i];
    }
    uint32_t user_envp_tab = (uint32_t)sp;

    // 压入 argv 指针数组 (以 NULL 结尾)
    sp--; *sp = 0; 
    for (int i = (int)argc - 1; i >= 0; i--) {
        sp--; *sp = k_argv_ps[i];
    }

    // 压入 argc。musl 的 _start 默认 *esp 是 argc
    sp--; *sp = argc;

    // 更新进程元数据
	// 更新进程名
    memcpy(cur->name, path_bk, TASK_NAME_LEN);
    cur->name[TASK_NAME_LEN - 1] = 0;

    // 准备中断栈，进入用户态
    struct intr_stack* intr_0_stack = (struct intr_stack*)((uint32_t)cur->kstack_pages + KERNEL_THREAD_STACK - sizeof(struct intr_stack));
    intr_0_stack->eip = (void*)entry_point;
    intr_0_stack->esp = (void*)sp;  // ESP 指向 argc
    
    // 按照 Linux ABI，EDX 存放 envp 地址，EBX 清零
    intr_0_stack->edx = user_envp_tab; 
    intr_0_stack->ebx = 0;
    intr_0_stack->ecx = 0;

    // 信号处理重置 (按照posix标准，要保留父进程的 SIG_IGN，其余的全部置为默认)
    for (int i=0; i < SIG_NR; i++) {
        if (cur->sigactions[i].sa_handler != SIG_IGN) {
            cur->sigactions[i].sa_handler = SIG_DFL;
        }
    }

	// 释放中转内存
    mfree_page(PF_KERNEL, k_arg_page, 1);
    kfree(path_bk);

    // 跳转执行
	// 切换栈并跳转到 intr_exit 执行 iret 进入用户态
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (intr_0_stack) : "memory");
    return 0;
}

// sys_execv 必须要有参数！至少要有第一个参数，即文件名！
// argv[0] 和 path 不总是一样，因为 argv[0] 可能是 ls ，而 path 是 /bin/ls
// 并且 argv 必须以 NULL 结尾
int32_t sys_execv(const char* path, const char* argv[]) {
    // 按照约定，不带 'e' 的版本使用当前进程的环境变量
    // 在这里我们暂时简单传一个空的 envp 数组
    const char* default_envp[] = { NULL };
    return sys_execve(path, argv, default_envp);
}