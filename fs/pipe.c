#include <stdint.h>
#include <pipe.h>
#include <memory.h>
#include <interrupt.h>
#include <ioqueue.h>
#include <debug.h>
#include <signal.h>
#include <errno.h>
#include <fs_types.h>
#include <file_table.h>
#include <inode.h>

int32_t sys_pipe(int32_t pipefd[2]) {

    // 创建一个匿名 Inode
    struct inode* inode = make_anonymous_inode();
    if (inode == NULL) {
        return -1;
    }

    inode->i_type = FT_PIPE;

    if (init_pipe(inode) != 0) {
        // 释放 inode，先 panic
        PANIC("failed to init_pipe");
        return -1;
    }

	struct pipe_inode_info* pii = (struct pipe_inode_info*)&inode->pipe_i;
    
    pii->reader_count = 1;
    pii->writer_count = 1;
    
    inode->i_type = FT_PIPE;
    inode->i_open_cnts = 2;

    // 尝试分配全局资源
    int32_t gfd_r = get_free_slot_in_global();
    if (gfd_r == -1) {
		// 释放 inode 和 p 然后报错 
		PANIC("sys_pipe:fail to get_free_slot_in_global for gfd_r!\n");
	}
	file_table[gfd_r].fd_inode = inode;
    file_table[gfd_r].fd_flag = O_RDONLY;
    // file_table[gfd_r].f_type = FT_PIPE;
    file_table[gfd_r].f_count = 1;      // 初始由当前进程的一个局部 FD 指向
    
    file_table[gfd_r].f_op = &pipe_file_operations;

    int32_t gfd_w = get_free_slot_in_global();
    if (gfd_w == -1) { 
		// 释放已占用的 gfd_r 等资源 
		PANIC("sys_pipe:fail to get_free_slot_in_global for gfd_w!\n");
	}
	file_table[gfd_w].fd_inode = inode;
    file_table[gfd_w].fd_flag = O_WRONLY;
    // file_table[gfd_w].f_type = FT_PIPE;
    file_table[gfd_w].f_count = 1;      // 初始由当前进程的一个局部 FD 指向

    file_table[gfd_w].f_op = &pipe_file_operations;

    // 映射到进程
    int32_t lfd_r = pcb_fd_install(gfd_r);
    int32_t lfd_w = pcb_fd_install(gfd_w);

    // 最终检查：如果局部 FD 满了
    if (lfd_r == -1 || lfd_w == -1) {
        // 回滚逻辑：close 掉已分配的 fd，释放 inode 和 p
		PANIC("sys_pipe:fail to pcb_fd_install!\n");
        return -1; 
    }

    pipefd[PIPE_READ] = lfd_r;
    pipefd[PIPE_WRITE] = lfd_w;

    return 0;
}

int32_t init_pipe(struct inode* inode) {
    // 关中断保护分配过程，防止竞态（尤其是 FIFO 多进程同时 open 时）
    enum intr_status old_status = intr_disable();

    // 如果已经有缓冲区了，直接返回成功（针对 FIFO 已经被别人打开的情况）
    if (inode->pipe_i.base != 0) {
        intr_set_status(old_status);
        return 0;
    }

    // 分配物理页
    // 申请一页内核内存作为 struct pipe (包含 ioqueue 和计数器)
	// 虽然 ioqueue 只占用 2048 字节，但是为了后期实现零拷贝
	// 比如通过页表重映射来实现管道数据传输, 需要满足页对齐的条件
	// 因此我们直接申请一个页的内存来存pipe
    struct pipe* p = (struct pipe*)get_kernel_pages(1);
    if (p == NULL) {
        intr_set_status(old_status);
        return -1;
    }

    ioqueue_init(&p->queue);
    inode->pipe_i.reader_count = 0;
    inode->pipe_i.writer_count = 0;
    inode->pipe_i.base = p;

    intr_set_status(old_status);
    return 0;
}

int32_t pipe_read(struct inode* inode UNUSED, struct file* file, char* buf, int32_t count) {
    struct pipe_inode_info* pii = (struct pipe_inode_info*)&file->fd_inode->pipe_i;
    struct pipe* p = pii->base;
    char* buffer = buf;
    int32_t bytes_read = 0;

    while (bytes_read < count) {
		// 每次检查前关中断，保护 ioq_empty 和 counts 的读取
        enum intr_status old_status = intr_disable();
        if (!ioq_empty(&p->queue)) {
            // 只要不空，就一直搬运数据
            buffer[bytes_read++] = ioq_get_raw(&p->queue);
			// 拿完一个就开中断，给其他任务机会，防止阻塞状态下死锁
			intr_set_status(old_status);
        } else {
            // 队列空了，开始判断策略

            // p->writer_count == 0) 写端全关了，返回 0 (EOF)
			// bytes_read > 0 如果已经读到一部分数据了，POSIX 要求立刻返回已读字节
            if (bytes_read > 0 || pii->writer_count == 0) {
                intr_set_status(old_status);
                break; 
            }

            // 非阻塞模式（未来支持），返回 -1
            // if (file->fd_flag & O_NONBLOCK) { bytes_read = -1; break; }

            // 默认阻塞模式，睡觉等数据
            ioq_wait(&p->queue.consumer);
			intr_set_status(old_status);
        }
    }

    return bytes_read;
}

int32_t pipe_write(struct inode* inode UNUSED, struct file* file, char* buf, int32_t count) {
    struct pipe_inode_info* pii = (struct pipe_inode_info*)&file->fd_inode->pipe_i;
    struct pipe* p = pii->base;
    const char* buffer = buf;
    int32_t bytes_written = 0;

    while (bytes_written < count) {
        enum intr_status old_status = intr_disable();
        
        // 每次循环先看一眼还有没有读者，这是 Broken Pipe 的根本检查点
        if (pii->reader_count == 0) {
            intr_set_status(old_status);
            // 发送 SIGPIPE 给当前进程
            send_signal(get_running_task_struct(), SIGPIPE);
            return -EPIPE; // 触发 SIGPIPE 逻辑
        }

        if (!ioq_full(&p->queue)) {
            ioq_put_raw(&p->queue, buffer[bytes_written++]);
            intr_set_status(old_status); 
            // 写入一个就开一次中断，给时钟中断/其他任务运行的机会
        } else {
            // 缓冲区满了
            // 再次确保读者还在，否则睡下去就醒不来了
            if (pii->reader_count == 0) {
                intr_set_status(old_status);
                return -EPIPE;
            }
            
            // 阻塞自己。调度器在切回来时应恢复 old_status 的中断状态
            ioq_wait(&p->queue.producer);
            intr_set_status(old_status);
        }
    }

    return bytes_written;
}

// release 函数只负责状态的维护，不负责缓冲区的释放
// 为保证资源管理的一致性，缓冲区的回收逻辑放到 inode_close 中进行
int32_t pipe_release(struct inode* inode, struct file* f) {
    ASSERT(inode==f->fd_inode);
    struct pipe_inode_info* pii = (struct pipe_inode_info*)&inode->pipe_i;
    struct pipe* p = pii->base;
    if(!p) return 0;

    // 严格根据读写权限减少计数
    if (f->fd_flag & O_RDONLY) {
        pii->reader_count--;
    }
    if (f->fd_flag & O_WRONLY) {
        pii->writer_count--;
    }

    // 唤醒逻辑，必须跨端唤醒，读端唤醒写端，写端唤醒读端
    // 如果关了读端，写端可能在等空间，必须唤醒写者，让它发现 reader_count == 0 (从而触发 Broken Pipe)
    if (p->queue.producer != NULL) {
        ioq_wakeup(&p->queue.producer);
    }
    // 如果关了写端，读端可能在等数据，必须唤醒读者，让它发现 writer_count == 0 (从而触发 EOF)
    if (p->queue.consumer != NULL) {
        ioq_wakeup(&p->queue.consumer);
    }

    // release 函数只负责状态的维护，不负责缓冲区的释放
    // 为保证资源管理的一致性，缓冲区的回收逻辑放到 inode_close 中进行

    return 0;
}

// 我们的 pipe 的写端和读端的 f_op 都是赋值的 pipe_file_operations
// 在Linux里面是分开的，有一个 pipe_read_operations 和 pipe_write_operations
// 读端write是NULL，写端read是NULL，我们先统一给这个，后期有时间再来优化
struct file_operations pipe_file_operations = {
	.lseek 		= NULL,
	.read 		= pipe_read,
	.write 		= pipe_write,
	.readdir 	= NULL,
	.ioctl 		= NULL,
	.open 		= NULL, // 匿名管道在文件系统上没有实体，所以没有open
	.release 	= pipe_release,
    .mmap		= NULL
};