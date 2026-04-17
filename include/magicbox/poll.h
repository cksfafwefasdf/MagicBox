#ifndef __INCLUDE_MAGICBOX_POLL_H
#define __INCLUDE_MAGICBOX_POLL_H

#include <dlist.h>

#define POLLIN      0x0001  // 有数据可读
#define POLLPRI     0x0002  // 有紧急数据可读
#define POLLOUT     0x0004  // 写数据不会阻塞
#define POLLERR     0x0008  // 指定的 FD 发生错误
#define POLLHUP     0x0010  // 指定的 FD 被挂断（如管道对端关闭）
#define POLLNVAL    0x0020  // 指定的 FD 无效

# define POLLRDNORM	0x040		/* Normal data may be read.  */
# define POLLRDBAND	0x080		/* Priority data may be read.  */
# define POLLWRNORM	0x100		/* Writing now will not block.  */
# define POLLWRBAND	0x200		/* Priority data may be written.  */

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

struct pollfd {
    int32_t fd;
    // events 用于存储用户关心的事件
    int16_t events;
    // r 代表 returned
    // 表示用户关系的事件中，哪些真的发生了
    int16_t revents;
};

extern void poll_wakeup(struct dlist* wait_list);
extern void poll_wait(struct file* filp, struct dlist* wait_list, struct poll_table* p);
extern int32_t sys_poll(struct pollfd* fds, uint32_t nfds, int32_t timeout_ms);

#endif