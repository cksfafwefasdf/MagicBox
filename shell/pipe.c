#include "stdbool.h"
#include "stdint.h"
#include "fs.h"
#include "file.h"
#include "pipe.h"
#include "ioqueue.h"
#include "thread.h"
#include "stdio.h"
#include "stdio-kernel.h"

bool is_pipe(uint32_t local_fd){
	uint32_t global_fd = fd_local2global(local_fd);
	return file_table[global_fd].fd_flag == PIPE_FLAG;
}

int sys_pipe(int32_t pipefd[2]){
	int32_t global_fd = get_free_slot_in_global();
	file_table[global_fd].fd_inode = get_kernel_pages(1);
	if(file_table[global_fd].fd_inode==NULL){
		return -1;
	}

	ioqueue_init((struct ioqueue*) file_table[global_fd].fd_inode);

	file_table[global_fd].fd_flag = PIPE_FLAG;

	file_table[global_fd].fd_pos = 2;
	pipefd[0] = pcb_fd_install(global_fd);
	pipefd[1] = pcb_fd_install(global_fd);
	return 0;
}

uint32_t pipe_read(int32_t fd,void* buf,uint32_t count){
	char* buffer = buf;
	uint32_t bytes_read = 0;
	uint32_t global_fd = fd_local2global(fd);
	
	struct ioqueue* ioq = (struct ioqueue*) file_table[global_fd].fd_inode;

	uint32_t ioq_len = ioq_length(ioq);
	uint32_t size = ioq_len>count?count:ioq_len;
	// printf("ioq_len: %d\n",ioq_len);
	// printf("count: %d\n",count);
	while(bytes_read<size){
		*buffer = ioq_getchar(ioq);
		// printk("bytes_read: %d\n",bytes_read);
		bytes_read++;
		buffer++;
	}
	printk("bytes_read: done!\n");
	return bytes_read;
}

uint32_t pipe_write(int32_t fd,const void* buf,uint32_t count){
	uint32_t bytes_write = 0;
	uint32_t global_fd = fd_local2global(fd);
	struct ioqueue* ioq = (struct ioqueue*) file_table[global_fd].fd_inode; 

	uint32_t ioq_left = BUFSIZE - ioq_length(ioq);
	uint32_t size = ioq_left>count?count:ioq_left;

	const char* buffer = buf;
	while(bytes_write<size){
		ioq_putchar(ioq,*buffer);
		bytes_write++;
		buffer++;
	}
	return bytes_write;
}

void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd){
	struct task_struct* cur = get_running_task_struct();
	
	if(new_local_fd<3){
		cur->fd_table[old_local_fd] = new_local_fd;
	}else{
		uint32_t new_global_fd = cur->fd_table[new_local_fd];
		cur->fd_table[old_local_fd] = new_global_fd;
	}
}
