#ifndef __USERPROG_VMA_H
#define __USERPROG_VMA_H

#include "stdbool.h"
#include "stdint.h"
#include "dlist.h"
#include "memory.h"

#define KERNEL_RESERVED_SPACE 0x100000UL
#define USER_VADDR_START 0x8048000UL
#define KERNEL_VADDR_START 0xC0000000UL
#define USER_STACK_SIZE 0x800000UL
// #define USER_STACK_BASE KERNEL_VADDR_START
#define USER_STACK_BASE 0xC0000000UL

#define VM_READ       0x0001
#define VM_WRITE      0x0002
#define VM_EXEC       0x0004
#define VM_STACK      0x0008  // 依然保留这个方便快速识别
#define VM_ANON       0x0010  // 匿名页（无文件备份，如堆、栈）
#define VM_GROWSDOWN  0x0020  // 向低地址生长（栈专用）
#define VM_GROWSUP    0x0040  // 向高地址生长（堆专用）

struct task_struct;

struct vm_area {
    uint32_t vma_start; // 虚拟地址起点
    uint32_t vma_end; // 虚拟地址终点，基于 memsz
    uint32_t vma_flags; // 权限（R/W/X/Heap）
	uint32_t vma_filesz; // 基于 filesz，用于区分文件数据和 BSS
    uint32_t vma_pgoff; // 对应文件中的偏移量
    struct m_inode* vma_inode; // 映射的文件（如果是匿名内存如堆栈，则为 NULL）
    struct dlist_elem vma_tag; // 用于挂载到 PCB 的链表
};


extern bool copy_vma_list(struct task_struct* parent, struct task_struct* child);
extern void remove_vma(struct vm_area* vma);
extern void add_vma(struct task_struct* task, uint32_t start, uint32_t end, uint32_t pgoff, struct m_inode* inode, uint32_t flags, uint32_t filesz); 
extern void add_vma_sorted(struct dlist* plist, uint32_t start, uint32_t end, 
             uint32_t pgoff, struct m_inode* inode, uint32_t flags, uint32_t filesz);
extern struct vm_area* find_vma(struct task_struct* task, uint32_t vaddr);
extern void clear_vma_list(struct task_struct* task);
extern uint32_t vma_find_gap(enum pool_flags pf, uint32_t pg_cnt);
extern struct vm_area* vma_split(struct vm_area* vma, uint32_t addr);
#endif