#ifndef __INCLUDE_MAGICBOX_POLL_H
#define __INCLUDE_MAGICBOX_POLL_H

#include <dlist.h>
#include <unitype.h>

struct task_struct;
struct file;

struct poll_table_entry {
    // 记录到底是谁在等待
    struct task_struct* task; 
    // 挂在 tty_struct->poll_waiters 上，让设备知道谁在监听它
    struct dlist_elem struct_tag; 
    // 挂在 sys_poll 的局部备忘录上，让进程能知道自己在监听哪些文件（设备）
    struct dlist_elem table_tag;  
};

struct poll_table {
    struct dlist nodes;             // 挂载所有 entry->table_tag 的备忘录
    struct poll_table_entry* array; // 本次 poll 预分配的 entry 数组
    uint32_t entry_count;           // 本次监听的 fd 数量 (nfds)
    uint32_t cur_index;             // 当前已经分配出去的 entry 索引
};

extern void poll_wakeup(struct dlist* wait_list);
extern void poll_wait(struct file* filp, struct dlist* wait_list, struct poll_table* p);
extern int32_t sys_poll(struct pollfd* fds, uint32_t nfds, int32_t timeout_ms);

#endif