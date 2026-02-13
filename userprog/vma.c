#include "vma.h"
#include "thread.h"
#include "inode.h"
#include "ide.h"
#include "debug.h"
#include "stdio-kernel.h"

// 判定函数，检查当前的 vma 是否包含目标地址
// arg 传入的是目标虚拟地址的指针
static bool vma_contains_address(struct dlist_elem* elem, void* arg) {
    uint32_t target_vaddr = *(uint32_t*)arg;

    struct vm_area* vma = member_to_entry(struct vm_area, vma_tag, elem);

    // 检查区间，左闭右开 [start, end)
    return (target_vaddr >= vma->vma_start && target_vaddr < vma->vma_end);
}

// 在 PCB 中找到 vaddr 相应的 vma
struct vm_area* find_vma(struct task_struct* task, uint32_t vaddr) {

    struct dlist_elem* vma_tag = dlist_traversal(&task->vma_list, vma_contains_address, &vaddr);

    if (vma_tag == NULL) {
        return NULL;
    }

    // 转换回 vm_area 指针并返回
    return member_to_entry(struct vm_area, vma_tag, vma_tag);
}

void add_vma(struct task_struct* task, uint32_t start, uint32_t end, 
             uint32_t pgoff, struct m_inode* inode, uint32_t flags, uint32_t filesz) {
    
    struct vm_area* vma = (struct vm_area*)kmalloc(sizeof(struct vm_area));
    if (vma == NULL) {
        PANIC("add_vma: kmalloc failed");
    }

    vma->vma_start = start;
    vma->vma_end = end;
    vma->vma_pgoff = pgoff;
    vma->vma_flags = flags;
    vma->vma_filesz = filesz;

    if (inode != NULL) {
        // 通过 inode 自己的设备号找回对应的分区
        struct partition* part = get_part_by_rdev(inode->i_dev);
        if (part == NULL) {
			printk("add_vma: partition not found for device %d\n", inode->i_dev);
            PANIC("partition not found!");
        }
        // 调用 inode_open，复用已有的 open_inodes 链表查找和 i_open_cnts++ 逻辑
        vma->vma_inode = inode_open(part, inode->i_no); 
    } else {
        vma->vma_inode = NULL; // 匿名映射
    }

    dlist_push_back(&task->vma_list, &vma->vma_tag);
}

void remove_vma(struct vm_area* vma) {
    if (vma == NULL) return;

    // 如果该 VMA 绑定了文件（非匿名映射）
    if (vma->vma_inode != NULL) {
        // 调用 inode_close。
        // 它会自动执行 i_open_cnts--，并在减到 0 时回收 inode 内存
        inode_close(vma->vma_inode);
    }

    // 从 PCB 的 vma_list 链表中摘除
    dlist_remove(&vma->vma_tag);

    // 释放 VMA 结构体本身占用的内核内存
    kfree(vma);
}

// 清空进程所有的 VMA 链表，execv 和 exit 中都要用，exit无需多言
// execv 中主要用于去清除继承父进程的 vma
void clear_vma_list(struct task_struct* task) {
    struct dlist_elem* elem = task->vma_list.head.next;

    // 循环遍历并销毁每一个 VMA
    while (elem != &task->vma_list.tail) {
        // 必须先记住下一个元素，因为 remove_vma 会把当前 elem 释放掉
        struct dlist_elem* next_elem = elem->next;

        // 使用你的 member_to_entry 宏找到 VMA 结构体
        struct vm_area* vma = member_to_entry(struct vm_area, vma_tag, elem);
        
        // 核心释放逻辑
        remove_vma(vma);

        elem = next_elem;
    }
}

// 在 fork 时克隆父进程的 VMA 链表给子进程，是深拷贝
bool copy_vma_list(struct task_struct* parent, struct task_struct* child) {
    struct dlist_elem* elem = parent->vma_list.head.next;

    while (elem != &parent->vma_list.tail) {
        struct vm_area* p_vma = member_to_entry(struct vm_area, vma_tag, elem);

        // 为子进程申请新的 VMA 结构体
        struct vm_area* c_vma = (struct vm_area*)kmalloc(sizeof(struct vm_area));
        if (c_vma == NULL) return false;

        // 复制基本属性
        memcpy(c_vma, p_vma, sizeof(struct vm_area));

        // 处理 inode 引用计数
        if (c_vma->vma_inode != NULL) {
            // 子进程也需要持有一份 inode 的引用。
            // 同样通过 rdev 反查分区并调用 inode_open，确保引用计数正确增加。
            struct partition* part = get_part_by_rdev(c_vma->vma_inode->i_dev);
            inode_open(part, c_vma->vma_inode->i_no);
        }

        // 挂载到子进程链表
        dlist_push_back(&child->vma_list, &c_vma->vma_tag);

        elem = elem->next;
    }
    return true;
}