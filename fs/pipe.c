#include "stdint.h"
#include "pipe.h"
#include "memory.h"
#include "interrupt.h"
#include "ioqueue.h"
#include "debug.h"
#include "inode.h"
#include "file.h"

int32_t sys_pipe(int32_t pipefd[2]) {
	// 申请一页内核内存作为 struct pipe (包含 ioqueue 和计数器)
	// 虽然 ioqueue 只占用 2048 字节，但是为了后期实现零拷贝
	// 比如通过页表重映射来实现管道数据传输, 需要满足页对齐的条件
	// 因此我们直接申请一个页的内存来存pipe
    struct pipe* p = (struct pipe*)get_kernel_pages(1);
    if (p == NULL) return -1;
    
    ioqueue_init(&p->queue);
    p->reader_count = 1;
    p->writer_count = 1;

	// 创建一个匿名 Inode
    struct m_inode* inode = make_anonymous_inode();
    if (inode == NULL) {
        mfree_page(PF_KERNEL,p, 1);
        return -1;
    }
    
    inode->di.i_type = FT_PIPE;
    inode->di.i_pipe_ptr = (uint32_t)p;
    inode->i_open_cnts = 2;

    // 尝试分配全局资源
    int32_t gfd_r = get_free_slot_in_global();
    if (gfd_r == -1) {
		// 释放 inode 和 p 然后报错 
		PANIC("sys_pipe:fail to get_free_slot_in_global for gfd_r!\n");
	}
	file_table[gfd_r].fd_inode = inode;
    file_table[gfd_r].fd_flag = O_RDONLY;
    file_table[gfd_r].f_type = FT_PIPE;
    file_table[gfd_r].f_count = 1;      // 初始由当前进程的一个局部 FD 指向
    
    int32_t gfd_w = get_free_slot_in_global();
    if (gfd_w == -1) { 
		// 释放已占用的 gfd_r 等资源 
		PANIC("sys_pipe:fail to get_free_slot_in_global for gfd_w!\n");
	}
	file_table[gfd_w].fd_inode = inode;
    file_table[gfd_w].fd_flag = O_WRONLY;
    file_table[gfd_w].f_type = FT_PIPE;
    file_table[gfd_w].f_count = 1;      // 初始由当前进程的一个局部 FD 指向

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
int32_t pipe_read(struct file* file, void* buf, uint32_t count) {
    struct pipe* p = (struct pipe*)file->fd_inode->di.i_pipe_ptr;
    char* buffer = buf;
    uint32_t bytes_read = 0;

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
            if (bytes_read > 0 || p->writer_count == 0) {
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

int32_t pipe_write(struct file* file, const void* buf, uint32_t count) {
    struct pipe* p = (struct pipe*)file->fd_inode->di.i_pipe_ptr;
    const char* buffer = buf;
    uint32_t bytes_written = 0;

    while (bytes_written < count) {
        enum intr_status old_status = intr_disable();
        
        // 每次循环先看一眼还有没有读者，这是 Broken Pipe 的根本检查点
        if (p->reader_count == 0) {
            intr_set_status(old_status);
            return -1; // 触发 SIGPIPE 逻辑，这在后期信号机制中实现
        }

        if (!ioq_full(&p->queue)) {
            ioq_put_raw(&p->queue, buffer[bytes_written++]);
            intr_set_status(old_status); 
            // 写入一个就开一次中断，给时钟中断/其他任务运行的机会
        } else {
            // 缓冲区满了
            // 再次确保读者还在，否则睡下去就醒不来了
            if (p->reader_count == 0) {
                intr_set_status(old_status);
                return -1;
            }
            
            // 阻塞自己。调度器在切回来时应恢复 old_status 的中断状态
            ioq_wait(&p->queue.producer);
            intr_set_status(old_status);
        }
    }

    return bytes_written;
}

void pipe_release(struct file* f) {
    struct m_inode* inode = f->fd_inode;
    struct pipe* p = (struct pipe*)inode->di.i_pipe_ptr;

    // 严格根据读写权限减少计数
    if (f->fd_flag & O_RDONLY) {
        p->reader_count--;
    }
    if (f->fd_flag & O_WRONLY) {
        p->writer_count--;
    }

    // 唤醒逻辑，必须跨端唤醒，读端唤醒写端，写端唤醒读端
    // 如果我关了读端，写端可能在等空间，必须唤醒写者，让它发现 reader_count == 0 (从而触发 Broken Pipe)
    if (p->queue.producer != NULL) {
        ioq_wakeup(&p->queue.producer);
    }
    // 如果我关了写端，读端可能在等数据，必须唤醒读者，让它发现 writer_count == 0 (从而触发 EOF)
    if (p->queue.consumer != NULL) {
        ioq_wakeup(&p->queue.consumer);
    }

    // 资源彻底回收
    if (p->reader_count == 0 && p->writer_count == 0) {
        mfree_page(PF_KERNEL,p, 1); // 释放 ioqueue 所在的物理页
        inode->di.i_pipe_ptr = 0; // 预防野指针
        // 匿名 Inode 的销毁不在此处进行，我们在sys_close中统一进行！
		// 该函数只释放与管道本身直接相关的结构
		
    }
}

