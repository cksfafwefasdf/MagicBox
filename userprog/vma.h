#ifndef __USERPROG_VMA_H
#define __USERPROG_VMA_H

#include "stdbool.h"
#include "stdint.h"
#include "dlist.h"

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
extern struct vm_area* find_vma(struct task_struct* task, uint32_t vaddr);
void clear_vma_list(struct task_struct* task); 
#endif