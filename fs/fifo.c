#include <fifo.h>
#include <pipe.h>
#include <fs_types.h>
#include <interrupt.h>

static int32_t fifo_open(struct inode* inode, struct file* file){
	// 如果 FIFO 还没分配内存缓冲区，则分配 (第一次打开)，该逻辑在 init_pipe 中处理
	// FIFO 本质上就只是 PIPE 在文件系统上的一个封装
	// PIPE 只能用于相互认识的进程之间的通信，比如 父进程和 fork 后的子进程
	// FIFO 用于解决相互不认识的进程之间的通信，只要知道FIFO的路径名
	// 这两个进程就能相互通信
	if (init_pipe(inode) != 0) {
		return -1;
	}
	struct pipe_inode_info* pii = (struct pipe_inode_info*)&inode->pipe_i;

	struct pipe* p = pii->base;

	// 根据当前打开标志更新 reader/writer 计数
	if (file->fd_flag & O_WRONLY || file->fd_flag & O_RDWR) pii->writer_count++;
	if (!(file->fd_flag & O_WRONLY) || file->fd_flag & O_RDWR) pii->reader_count++;

	// 处理 POSIX 阻塞逻辑
	// 如果是读打开，循环等待直到有写者，如果是写打开，循环等待直到有读者。
	enum intr_status old_status = intr_disable();
	if (file->fd_flag & O_WRONLY) {
		// 对于写者，如果当前没读者，要睡在生产者队列等读者来
		// 使用 while 是为了防止虚假唤醒。
		// 比如一个读者刚被唤醒，结果唯一的写者突然闪退了（或者被信号杀掉了）
		// 此时读者必须再次检查 writer_count，如果还是 0，就得继续睡。
		while (pii->reader_count == 0) {
			// 唤醒可能正在 open 阶段等待写者的读者
			if (p->queue.consumer != NULL) ioq_wakeup(&p->queue.consumer);
			
			ioq_wait(&p->queue.producer); // 阻塞自己
		}
		// 成功等到读者，唤醒它
		if (p->queue.consumer != NULL) ioq_wakeup(&p->queue.consumer);
		
	} else {
		// 对于读者，如果当前没写者，要睡在消费者队列等写者来
		while (pii->writer_count == 0) {
			// 唤醒可能正在 open 阶段等待读者的写者
			if (p->queue.producer != NULL) ioq_wakeup(&p->queue.producer);
			
			ioq_wait(&p->queue.consumer); // 阻塞自己
		}
		// 成功等到写者，唤醒它
		if (p->queue.producer != NULL) ioq_wakeup(&p->queue.producer);
	}
	intr_set_status(old_status);
	return 0;
}

static int32_t fifo_release(struct inode* inode, struct file* file){
	return pipe_release(inode ,file);
}

static int32_t fifo_read(struct inode* inode, struct file* file, char* buf, int32_t count){
	return pipe_read(inode,file, buf, count);
}

static int32_t fifo_write(struct inode* inode, struct file* file, char* buf, int32_t count){
	return pipe_write(inode,file,buf,count);
}

struct file_operations fifo_file_operations = {
	.lseek 		= NULL,
	.read 		= fifo_read,
	.write 		= fifo_write,
	.readdir 	= NULL,
	.ioctl 		= NULL,
	.open 		= fifo_open,
	.release 	= fifo_release
};
