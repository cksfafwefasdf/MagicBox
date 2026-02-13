#include "thread.h"
#include "stdint.h"
#include "stdbool.h"
#include "fs.h"
#include "dlist.h"
#include "wait_exit.h"
#include "file.h"
#include "debug.h"
#include "memory.h"
#include "process.h"

struct wait_opts {
    pid_t target_pid;
    pid_t parent_pid;
};

static void release_prog_resource(struct task_struct* release_thread){
	release_pg_block(release_thread);

	uint32_t bitmap_pg_cnt = (release_thread->userprog_vaddr.vaddr_bitmap.btmp_bytes_len)/PG_SIZE;
	uint8_t* user_vaddr_pool_bitmap = release_thread->userprog_vaddr.vaddr_bitmap.bits;
	mfree_page(PF_KERNEL,user_vaddr_pool_bitmap,bitmap_pg_cnt);
	uint8_t local_fd = 0;
	while(local_fd<MAX_FILES_OPEN_PER_PROC){
		if(release_thread->fd_table[local_fd]!=-1){
			// 无论是不是管道，sys_close都可以统一处理
			sys_close(local_fd);
		}
		local_fd++;
	}
}

static bool find_child(struct dlist_elem* pelem,void* arg){
	int32_t ppid = (int32_t)arg;
	struct task_struct* pthread = member_to_entry(struct task_struct,all_list_tag,pelem);
	if(pthread->parent_pid==ppid){
		return true;
	}
	return false;
}

static bool find_hanging_child(struct dlist_elem* pelem,void* arg){
	int32_t ppid = (int32_t)arg;
	struct task_struct* pthraed = member_to_entry(struct task_struct,all_list_tag,pelem);
	if(pthraed->parent_pid==ppid&&pthraed->status==TASK_HANGING){
		return true;
	}
	return false;
}

// 托孤代码，如果该进程调用了exit，先将其子进程全部挂到init下
static bool init_adopt_a_child(struct dlist_elem* pelem,void* arg){
	int32_t pid = (int32_t)arg;
	struct task_struct* pthread = member_to_entry(struct task_struct,all_list_tag,pelem);
	if(pthread->parent_pid==pid){
		pthread->parent_pid = 3; // init 的 pid 是 3
	}
	return false;
}


pid_t sys_wait(int32_t* status){
	struct task_struct* parent_thread = get_running_task_struct();

	while(1){
		struct dlist_elem* child_elem = dlist_traversal(&thread_all_list,find_hanging_child,(void*)parent_thread->pid);
		if(child_elem!=NULL){
			struct task_struct* child_thread = member_to_entry(struct task_struct,all_list_tag,child_elem);
			if(status!=NULL)
				*status = child_thread->exit_status;

			uint16_t child_pid = child_thread->pid;
			release_pg_table(child_thread);
			release_pg_dir(child_thread);
			thread_exit(child_thread,false);
			return child_pid;
		}

		child_elem = dlist_traversal(&thread_all_list,find_child,(void*)parent_thread->pid);
		if(child_elem == NULL){
			return -1;
		}else{
			thread_block(TASK_WAITING);
		}
	}
}

void sys_exit(int32_t status){
	struct task_struct* child_thread = get_running_task_struct();
	child_thread->exit_status = status;
	if(child_thread->parent_pid==-1){
		PANIC("sys_exit: child_thread->parent_pid is -1\n");
	}

	dlist_traversal(&thread_all_list,init_adopt_a_child,(void*)child_thread->pid);

	release_prog_resource(child_thread);

	struct task_struct* parent_thread = pid2thread(child_thread->parent_pid);

	// 发送信号，实现异步回收
	// 如果没有信号的话，在先前的实现中，只有父进程调用wait了，他才能去回收子进程
	// 但是现在加入了信号，即使父进程没有wait，它只要收到信号了，在信号处理的过程中也会去回收子进程
	send_signal(parent_thread, SIGCHLD);

	if(parent_thread->status==TASK_WAITING){
		thread_unblock(parent_thread);
	}

	// 检查是否有已经变成僵尸的孩子交给了 Init
    // 如果有，唤醒 PID 3
    struct task_struct* init_proc = pid2thread(3); // Init 是 PID 3
    if (init_proc) {
        // 扫描全进程表，看看有没有 parent 是 3 且状态是 HANGING 的
        struct dlist_elem* zombie_elem = dlist_traversal(&thread_all_list, find_hanging_child, (void*)3);
        if (zombie_elem != NULL) {
            // 发现有过继过来的僵尸，立刻叫醒 Init
            send_signal(init_proc, SIGCHLD);
            if (init_proc->status == TASK_WAITING) {
                thread_unblock(init_proc);
            }
        }
    }

	thread_block(TASK_HANGING);
}


static bool find_specific_child(struct dlist_elem* pelem, void* arg) {
    struct wait_opts* opts = (struct wait_opts*)arg;
    struct task_struct* pthread = member_to_entry(struct task_struct, all_list_tag, pelem);
    
    // 基本条件，必须是我的孩子
    if (pthread->parent_pid != opts->parent_pid) return false;
    
    // 如果 target_pid 是 -1，代表回收任意孩子
    if (opts->target_pid == -1) return true;
    
    // 否则，必须匹配特定的 PID
    return pthread->pid == opts->target_pid;
}

static bool find_specific_hanging_child(struct dlist_elem* pelem, void* arg) {
    struct wait_opts* opts = (struct wait_opts*)arg;
    struct task_struct* pthread = member_to_entry(struct task_struct, all_list_tag, pelem);
    
    // 基本条件，必须是我的孩子
    if (pthread->parent_pid != opts->parent_pid) return false;
    
	// 基本条件，必须是挂起态
	if (pthread->status != TASK_HANGING) return false;

	// 如果 target_pid 是 -1，代表任意孩子
    if (opts->target_pid == -1) return true;
    
    // 否则，必须匹配特定的 PID
    return pthread->pid == opts->target_pid;
}

// 只回收 pid 等于参数值的进程
// options 用于控制回收的方式，异步还是同步
pid_t sys_waitpid(pid_t pid, int32_t* status, int32_t options) {
    struct task_struct* parent_thread = get_running_task_struct();
    struct wait_opts opts = { .target_pid = pid, .parent_pid = parent_thread->pid };

    while(1) {
        // 查找是否有符合条件的、已经挂起的子进程 (TASK_HANGING)，调用sys_exit后会处于TASK_HANGING态
        struct dlist_elem* child_elem = dlist_traversal(&thread_all_list, find_specific_hanging_child, &opts);
        
        if (child_elem != NULL) {
            struct task_struct* child_thread = member_to_entry(struct task_struct, all_list_tag, child_elem);
            if (status != NULL) *status = child_thread->exit_status;
            uint16_t child_pid = child_thread->pid;
			release_pg_table(child_thread); // 释放页表
			release_pg_dir(child_thread); // 释放页目录表
            thread_exit(child_thread, false); // 彻底销毁
            return child_pid;
        }

        // 如果没找到挂起的，看看有没有符合条件的、还活着的子进程
        child_elem = dlist_traversal(&thread_all_list, find_specific_child, &opts);
        
        if (child_elem == NULL) {
            return -1; // 根本没有符合条件的子进程
        } else {
            // 如果设置了 WNOHANG，且孩子还没死，直接返回 0
            if (options & WNOHANG) {
                return 0; 
            }
            // 否则，老老实实阻塞等待
            thread_block(TASK_WAITING);
        }
    }
}