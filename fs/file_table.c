#include <file_table.h>
#include <string.h>
#include <fs_types.h>
#include <debug.h>
#include <stdio-kernel.h>

// System-wide Open File Table
struct file file_table[MAX_FILE_OPEN_IN_SYSTEM];

int32_t get_free_slot_in_global(void){
	uint32_t fd_idx = 3;
	while(fd_idx<MAX_FILE_OPEN_IN_SYSTEM){
		if(file_table[fd_idx].fd_inode==NULL){
			// 找到了就地清空，防止残留脏数据
            memset(&file_table[fd_idx], 0, sizeof(struct file)); 
            break;
		}
		fd_idx++;
	}
	if(fd_idx==MAX_FILE_OPEN_IN_SYSTEM){
		printk("exceed max open files!\n");
		return -1;
	}
	return fd_idx;
}

int32_t pcb_fd_install(int32_t global_fd_idx){
	struct task_struct* cur = get_running_task_struct();

	uint8_t local_fd_idx = 0;
	while(local_fd_idx<MAX_FILES_OPEN_PER_PROC){
		if(cur->fd_table[local_fd_idx]==-1){
			cur->fd_table[local_fd_idx]=global_fd_idx;
			break;
		}
		local_fd_idx++;
	}

	if(local_fd_idx==MAX_FILES_OPEN_PER_PROC){
		printk("exceed max open file_per_proc!\n");
		return -1;
	}
	return local_fd_idx;
}

uint32_t fd_local2global(struct task_struct* task , uint32_t local_fd){
	int32_t global_fd = task->fd_table[local_fd];
	ASSERT(global_fd>=0&&global_fd<MAX_FILE_OPEN_IN_SYSTEM);
	return (uint32_t)global_fd;
}