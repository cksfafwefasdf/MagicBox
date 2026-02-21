#include "vma.h"
#include "thread.h"
#include "inode.h"
#include "ide.h"
#include "debug.h"
#include "stdio-kernel.h"
#include "memory.h"

// 判定函数，检查当前的 vma 是否包含目标地址
// arg 传入的是目标虚拟地址的指针
static bool vma_contains_address(struct dlist_elem* elem, void* arg) {
    uint32_t target_vaddr = *(uint32_t*)arg;

    struct vm_area* vma = member_to_entry(struct vm_area, vma_tag, elem);

#ifdef DEBUG_VMA
    printk("%x-%x:%x->", vma->vma_start, vma->vma_end,vma->vma_flags);
#endif

    // 检查区间，左闭右开 [start, end)
    return (target_vaddr >= vma->vma_start && target_vaddr < vma->vma_end);
}

// 在 PCB 中找到 vaddr 相应的 vma
struct vm_area* find_vma(struct task_struct* task, uint32_t vaddr) {
#ifdef DEBUG_VMA
    printk("find_vma:vaddr: %x => ",vaddr);
#endif
    struct dlist_elem* vma_tag = dlist_traversal(&task->vma_list, vma_contains_address, &vaddr);

#ifdef DEBUG_VMA
    printk("\n");
#endif
    

    
    if (vma_tag == NULL) {
        return NULL;
    }

    // 转换回 vm_area 指针并返回
    return member_to_entry(struct vm_area, vma_tag, vma_tag);
}

// 谓词函数，找到第一个起始地址大于新 VMA 起始地址的元素
static bool vma_after_address(struct dlist_elem* elem, void* arg) {
    uint32_t new_start = *(uint32_t*)arg;
    struct vm_area* vma = member_to_entry(struct vm_area, vma_tag, elem);
    return vma->vma_start > new_start;
}

// 该函数会按照地址从小到大有序插入 vma
// 该函数不进行任务绑定，只是单纯的插入
// 以便于在内核态下也可以对这个函数进行调用
void add_vma_sorted(struct dlist* plist, uint32_t start, uint32_t end, 
             uint32_t pgoff, struct m_inode* inode, uint32_t flags, uint32_t filesz) {

    // 寻找插入位置（第一个起始地址比待插元素大的元素）
    struct dlist_elem* next_elem = dlist_traversal(plist, vma_after_address, &start);
    struct dlist_elem* prev_elem = (next_elem == NULL) ? plist->tail.prev : next_elem->prev;

    struct vm_area *prev_vma = (prev_elem == &plist->head) ? NULL : member_to_entry(struct vm_area, vma_tag, prev_elem);
    struct vm_area *next_vma = (next_elem == NULL) ? NULL : member_to_entry(struct vm_area, vma_tag, next_elem);      
    
    // 尝试向后合并（和 prev 融合）
    // 需要检查是否是指向同一个inode，是否具有相同的权限，地址是否相接
    // 此外，对于非匿名映射（绑定了文件的映射），还要检查段在文件中的偏移量是否连续
    // 比如假设程序先 mmap 了文件的 0-4KB 到地址 A，紧接着又 mmap 了同一个文件的 1MB-1.004MB 到地址 A+4KB。
    // 此时地址相接，inode 相同，权限相同。
    // 如果合并了：当访问 A+4KB 时，swap_page 会计算偏移量，误以为你要读取文件 4KB-8KB 的内容，从而导致读取数据错误。
    if (prev_vma && prev_vma->vma_end == start && 
        prev_vma->vma_flags == flags && prev_vma->vma_inode == inode &&
        (inode == NULL || prev_vma->vma_pgoff + (prev_vma->vma_end - prev_vma->vma_start) / PG_SIZE == pgoff)) {
        // 若能接上，那么单纯的推高前一个vma的end就行
        prev_vma->vma_end = end;
        
        // 既然和 prev 合并了，那现在能不能和 next 也连上，即三合一
        if (next_vma && end == next_vma->vma_start && 
            next_vma->vma_flags == flags && next_vma->vma_inode == inode) {
            // 如果可以接上，那么就把前者的end进一步后推
            // 并将后者删除
            prev_vma->vma_end = next_vma->vma_end;
            remove_vma(next_vma);
        }
        return; // 合并成功，不需要新建
    }

    // 尝试向前合并（和 next 融合）
    if (next_vma && next_vma->vma_start == end && 
        next_vma->vma_flags == flags && next_vma->vma_inode == inode) {
        
        next_vma->vma_start = start;
        return; // 合并成功
    }

    // 无法合并，不得不创建
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

    struct dlist_elem* before_me = dlist_traversal(plist, vma_after_address, &start);

    if (before_me == NULL) {
        // 没找到比我大的，说明我是目前地址最大的，插在末尾
        dlist_push_back(plist, &vma->vma_tag);
    } else {
        // 插在第一个比我大的元素前面
        dlist_insert_front(before_me, &vma->vma_tag);
    }
}

// 任务绑定, 给特定进程添加 VMA
void add_vma(struct task_struct* task, uint32_t start, uint32_t end, uint32_t pgoff, struct m_inode* inode, uint32_t flags, uint32_t filesz) {
    add_vma_sorted(&task->vma_list, start, end, pgoff, inode, flags, filesz);
}

void remove_vma(struct vm_area* vma) {
    if (vma == NULL) return;

#ifdef DEBUG_VMA
    printk("DEBUG: Removing VMA: start=%x, end=%x, vma_ptr=%x\n", vma->vma_start, vma->vma_end, vma);
#endif

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

// 从搜索起点开始，挨个对比相邻 VMA 之间的缝隙。
// 仅做虚拟地址的区间搜索，不建立实际映射
// 用于替代原本实现中的虚拟地址位图扫描搜索
uint32_t vma_find_gap(struct task_struct* task ,uint32_t pg_cnt) {
    uint32_t size = pg_cnt * PG_SIZE;
    
    // 确定边界，用户态从 0x08048000 开始
    uint32_t search_ptr = USER_VADDR_START;
    uint32_t upper_limit = USER_STACK_BASE - USER_STACK_SIZE; // 避开栈
    
    struct dlist* plist = &task->vma_list;
    
    if (dlist_empty(plist)) return search_ptr;

    struct dlist_elem* elem = plist->head.next;
    while (elem != &plist->tail) {
        struct vm_area* vma = member_to_entry(struct vm_area, vma_tag, elem);
        ASSERT(vma->vma_end%4096==0);
        
        
        // 检查当前 search_ptr 和当前 vma 起始地址之间的 Gap
        if (vma->vma_start > search_ptr && (vma->vma_start - search_ptr) >= size) {
            return search_ptr;
        }
        
        // 如果没空间，跳到当前 vma 的结束位置继续往后找
        if (vma->vma_end > search_ptr) {
            search_ptr = vma->vma_end;
        }
        elem = elem->next;
    }

    // 检查最后一个 VMA 到上限之间的空间
    if (upper_limit - search_ptr >= size) {
        return search_ptr;
    }

    return 0; // 没空间了
}

// 将一个 VMA 从 addr 处切断，分裂成两个
// 这个函数会在 vaddr_remove 这种挖洞的场景下被调用
struct vm_area* vma_split(struct vm_area* vma, uint32_t addr) {

    // 用于经过防止堆串孔
    if (vma->vma_flags & VM_GROWSUP) {
        printk("WARNING: Heap VMA is being split! (This should not happen in do_free)\n");
    }

    // 申请新 VMA 结构
    struct vm_area* new_vma = (struct vm_area*)kmalloc(sizeof(struct vm_area));
    if (new_vma == NULL){
        return NULL;
    } 

    // 拷贝属性并裁剪
    new_vma->vma_start = addr;
    new_vma->vma_end = vma->vma_end;
    new_vma->vma_flags = vma->vma_flags;
    // 计算后半部分在文件中的偏移
    new_vma->vma_pgoff = vma->vma_pgoff + (addr - vma->vma_start) / PG_SIZE;
    new_vma->vma_filesz = vma->vma_filesz; // 简单拷贝

    if (vma->vma_inode) {
        // 增加文件引用计数
        struct partition* part = get_part_by_rdev(vma->vma_inode->i_dev);
        new_vma->vma_inode = inode_open(part, vma->vma_inode->i_no); 
    } else {
        new_vma->vma_inode = NULL;
    }

    // 调整原 VMA 边界
    vma->vma_end = addr;

    // 插入链表
    // 在 vma->next 之前插入，逻辑上就是紧跟在 vma 后面
    dlist_insert_front(vma->vma_tag.next, &new_vma->vma_tag);

    return new_vma;
}