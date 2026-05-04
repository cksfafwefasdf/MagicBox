#ifndef __INCLUDE_MAGICBOX_EXEC_H
#define __INCLUDE_MAGICBOX_EXEC_H

#include <stdint.h>

#define MAX_PT_LOADER_SEGMENT 10 // the max number of the PT_LOAD segment is 10 (Linux standard)

// 终止符，用于告诉 C 库或动态链接器辅助向量表到此结束，类似于字符串结尾的 \0
#define AT_NULL   0 
// 指向主程序程序头表在虚拟内存中的地址，动态链接器（ld.so）必须读取程序头表来定位 .dynamic 段，从而知道主程序依赖哪些库。
#define AT_PHDR   3 
// 每个程序头条目的大小（在 32 位系统上通常是 32 字节），用于遍历 AT_PHDR 指向的数组。
#define AT_PHENT  4 
// 程序头表中有多少个条目，防止 ld.so 在遍历程序头时越界。
#define AT_PHNUM  5 
// 系统的物理页大小（通常是 4096，即 4KB），malloc 等内存分配函数需要知道页对齐边界；动态链接器在映射共享库时也需要按页对齐。
#define AT_PAGESZ 6 
// 动态链接器（Interpreter）本身被加载到的基地址，如果是静态链接程序，这个值为 0。如果是动态链接，它告诉内核 ld.so 映射在哪个偏置位置。
#define AT_BASE   7  
// Entry Point，主程序的入口点地址（即 elf_header.e_entry）
// 当动态链接器（ld.so）帮主程序搬完砖（加载完所有 .so 库）后，它需要知道跳到哪里去开始执行主程序，这个地址就从这里拿。
#define AT_ENTRY  9 

typedef uint32_t Elf32_Word,Elf32_Addr,Elf32_Off;
typedef uint16_t Elf32_Half;

struct Elf32_Ehdr{
	unsigned char e_ident[16];
	Elf32_Half e_type;
	Elf32_Half e_machine;
	Elf32_Word e_version;
	Elf32_Addr e_entry;
	Elf32_Off e_phoff;
	Elf32_Off e_shoff;
	Elf32_Word e_flags;
	Elf32_Half e_ehsize;
	Elf32_Half e_phentsize;
	Elf32_Half e_phnum;
	Elf32_Half e_shentsize;
	Elf32_Half e_shnum;
	Elf32_Half e_shstrndx;
};

struct Elf32_Phdr{
	Elf32_Word p_type;
	Elf32_Off p_offset;
	Elf32_Addr p_vaddr;
	Elf32_Addr p_paddr;
	Elf32_Word p_filesz;
	Elf32_Word p_memsz;
	Elf32_Word p_flags;
	Elf32_Word p_align;
};
// p_type: 段的类型
enum segment_type{
	PT_NULL,
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR
};

// p_flags: 段的权限 (按位组合)
enum segment_flags {
    PF_X = 1, // 可执行 (Executable)
    PF_W = 2, // 可写 (Writable)
    PF_R = 4  // 可读 (Readable)
};

extern int32_t sys_execv(const char* path,const char* argv[]);
extern int32_t sys_execve(const char* path, const char* argv[], const char* envp[]);
#endif