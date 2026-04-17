#include <poll.h>
#include <stdint.h>
#include <timer.h>
#include <file_table.h>
#include <fs_types.h>
#include <signal.h>
#include <thread.h>
#include <errno.h>
#include <interrupt.h>
#include <stdio-kernel.h>

// 遍历在 do_poll 期间创建的所有 poll_table_entry
// 并将它们从各个设备的等待队列中彻底移除。
static void poll_cleanup(struct poll_table* pt) {
    enum intr_status old_status = intr_disable(); // 必须关中断防止并发问题
    struct dlist_elem* elem = pt->nodes.head.next;

    while (elem != &pt->nodes.tail) {
        // 通过 table_tag 找到对应的 entry 结构体
        struct poll_table_entry* entry = member_to_entry(struct poll_table_entry, table_tag, elem);
        
        // 保存下一个节点，因为 dlist_remove 会破坏当前节点的 next 指针
        struct dlist_elem* next_node = elem->next;

        // 将该 entry 从设备的 poll_entry_list 中移除
        // dlist_remove 只需要知道 elem 就能把它从它所在的任何链表中摘除
        // 所以我们不需要知道这个 entry 到底挂在哪个 TTY 或哪个串口上
        if (dlist_is_linked(&entry->struct_tag)) {
            dlist_remove(&entry->struct_tag);
        }

        // 将该 entry 从 poll_table 的备忘录中移除
        // 可以不做，因为后面会释放整个 array，但是做一下更清晰一些
        dlist_remove(&entry->table_tag);

        elem = next_node;
    }

    intr_set_status(old_status);
}

// 用户传入一个 pollfd 数组表示监听哪些文件，以及相应的事件
// poll 的语义是监听一堆任务，只要其中任意一个完成就返回提醒用户
// 如果同时监听多个任务的话，可能会有一些任务还没处理好呢 poll 就返回了
// 此时用户就先处理那个准备好了的任务，之后再调用 poll 继续监听
// 因此 poll 通常都是在一个 循环中被反复调用的
// 但是正如代码所示的那样，poll 每次都要“重新登记、重新清空”
// 当监听的 FD 达到几万个时，这种反复搬运和扫描的开销就会变得非常恐怖。
// 因此后面才会出现 epoll
static int32_t do_poll(struct pollfd* fds, uint32_t nfds, uint32_t timeout_ms) {
    struct poll_table table;
    table.array = kmalloc(sizeof(struct poll_table_entry) * nfds);
    table.entry_count = nfds;
    table.cur_index = 0;
    dlist_init(&table.nodes);

    int32_t ready_count = 0;
    struct task_struct* cur = get_running_task_struct();
    bool first_pass = true; // 标记是否是第一次扫描

    while (1) {
        ready_count = 0;
        // 遍历监听的文件
        for (uint32_t i = 0; i < nfds; i++) {
            // 如果 fd < 0，POSIX 要求忽略它
            if (fds[i].fd < 0) { fds[i].revents = 0; continue; }

            struct file* file = &file_table[fd_local2global(cur, fds[i].fd)];
            if (!file) {
                continue;
            }

            if(!file->f_op->poll){
                printk("do_poll: this type of file (%d) cannot poll!\n",file->fd_inode->i_type);
                continue;
            }

            // 只有真正意义上的第一次扫描才传入 table 进行“挂钩”
            // 之后无论 ready_count 是否为 0，都不再传入 table
            // 文件系统层的 poll 函数负责将当前的这个 table 上的有效 entry 
            // 挂到文件的 poll 队列上
            // poll 队列是隐藏在具体的设备或者文件的结构体中的
            // 而不是在 vfs 层的 file 结构体中
            // 因为绝大多数的文件还是普通文件，他根本用不到 poll 队列，用到 poll 的是少数
            // 而且这个队列基本只在 poll 函数中访问
            // 一个文件或者说设备既然实现了 poll，那么它肯定也会给自己实现相应的 poll 队列，
            // 他自己也知道怎么操作自己的poll队列，因此我们将poll队列下放到具体的设备节点中
            uint32_t mask = file->f_op->poll(file, (first_pass ? &table : NULL));
            
            // mask 会返回当前完成了的事件，只要任意一个事件完成了，ready_count 就会增加
            // 用户设置 events = POLLIN | POLLOUT（关心读或写）。
            // 现在文件可读但不可写。那么 revents 变为 POLLIN，ready_count 增加。
            // 文件既可读又可写。那么 revents 变为 POLLIN | POLLOUT，ready_count 同样增加。
            // ready_count 统计的是“有多少个 fd 处于就绪状态”，而不是“有多少个事件完成了”。
            // 拿 read 举例，用户在检查了 revents 的事件 POLLIN 后
            // 如果为真，才会去真的 read
            // POLLERR POLLHUP POLLNVAL 都是一些异常状态
            // 为了防止用户在发生异常后死循环，这些事件就算用户不订阅也要返回
            fds[i].revents = mask & (fds[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (fds[i].revents) {
                ready_count++;
            }
        }
        first_pass = false; // 第一轮扫完，钩子已经挂好了，以后只查状态
        
        // printk("sys_poll: ready_cnt:%d,timeoutms:%d,sig:0x%x\n",ready_count,timeout_ms,signal_pending(cur));

        // 检查退出：有货了、超时了、或者有信号了
        if (ready_count > 0 || timeout_ms == 0 || signal_pending(cur)) {
            break;
        }

        // 千万不要忘记处理这个 -1 ！！！
        // 因为 ash 在初始化的时候会发送一个 -1 的 timeout 来把自己阻塞起来
        // 直到有字符才把自己唤醒！如果不处理-1的话 ash 会直接自动退出！
        if (timeout_ms == 0xffffffff) { // 即 -1
            // 不挂定时器，只阻塞，进入无限阻塞模式，等待 poll_wakeup 唤醒
            enum intr_status old_status = intr_disable();
            thread_block(TASK_WAITING); 
            intr_set_status(old_status);
        } else {
            // 正常的定时等待
            // 睡觉，返回剩余毫秒数
            // 我们在 sys_milsleep 会将节点挂到睡眠节点上，而不是在就绪队列上
            timeout_ms = sys_milsleep(timeout_ms);
        }
    }

    // 清理所有挂载在驱动上的 entries
    poll_cleanup(&table);
    kfree(table.array);

    // 如果是信号中断，返回 -EINTR
    if (ready_count == 0 && signal_pending(cur)) {
        return -EINTR;
    }
    
    return ready_count;
}

int32_t sys_poll(struct pollfd* fds, uint32_t nfds, int32_t timeout_ms) {
    if (nfds > MAX_FILES_OPEN_PER_PROC) return -EINVAL;
    if (!fds) return -EFAULT;

    return do_poll(fds, nfds, (uint32_t)timeout_ms);
}

// 用于挂钩子
// 将需要等待的 entry 挂到 poll 队列上 
void poll_wait(struct file* filp UNUSED, struct dlist* wait_list, struct poll_table* p) {
    // 只有在 p 不为空，且驱动提供了队列头，且 table 还没满的情况下才挂载
    if (p && wait_list && p->cur_index < p->entry_count) {
        struct poll_table_entry* entry = &p->array[p->cur_index++];

        // 记录当前的 task
        entry->task = get_running_task_struct();
        
        // 双向挂载
        // 挂到设备的 poll 列表里，为了让中断能找到进程
        dlist_push_back(wait_list, &entry->struct_tag);
        
        // 挂到 poll_table 的 nodes 里，为了让 poll_cleanup 能清理进程
        dlist_push_back(&p->nodes, &entry->table_tag);
    }
}

// 遍历驱动的 poll 等待队列，唤醒所有阻塞在 do_poll 里的进程
// 这可能会出现惊群效应，假如有多个进程订阅同一个事件的话
// 一个条件满足了会把所有进程都唤醒，例如在读 tty 时，有多个进程都订阅了读就绪
// 一旦都就绪后直接唤醒了所有进程，假如一个进程把数据一次性全读完了那其他进程就没得读了
// 这种情况只能交给用户态程序自己去竞争，例如在读 tty 时，若出现了 EAGAIN
// 那就说明出现了上面那种情况，当前进程就得重新去读，这确实有点低效，但这是 poll 操作的必然
// epoll 引入了 边缘触发 (Edge Triggered) 和特殊的标记位（如 EPOLLEXCLUSIVE），专门用来解决这种“唤醒所有人”的低效问题，确保只唤醒一个或少数几个进程。
void poll_wakeup(struct dlist* wait_list) {
    if (!wait_list || dlist_empty(wait_list)) {
        return;
    }

    enum intr_status old_status = intr_disable(); // 关中断保护链表操作

    struct dlist_elem* elem = wait_list->head.next;
    while (elem != &wait_list->tail) {
        // 通过 struct_tag 找到 entry，进而拿到 task
        struct poll_table_entry* entry = member_to_entry(struct poll_table_entry, struct_tag, elem);
        
        struct task_struct* task = entry->task;

        // 唤醒进程
        // 只要进程目前处于 TASK_WAITING（正在 sys_milsleep 睡眠中），就将其改为就绪态
        // thread_unblock 会将任务放入就绪队列，do_poll 里的 sys_milsleep 就会返回
        // sys_milsleep 后半段流程会处理 timer_tag 的出队，因此我们在此处不需要特别处理
        if (task->status == TASK_WAITING) {
            thread_unblock(task);
        }

        elem = elem->next;
    }

    intr_set_status(old_status);
}