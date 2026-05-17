#include <thread.h>
#include <stdint.h>
#include <stdbool.h>
#include <fs.h>
#include <dlist.h>
#include <wait_exit.h>
#include <debug.h>
#include <memory.h>
#include <process.h>
#include <vma.h>
#include <file_table.h>
#include <inode.h>
#include <stdio-kernel.h>

struct wait_opts {
    pid_t target_pid;
    pid_t parent_pid;
};

static void release_files(struct task_struct* release_thread) {
    if (release_thread->file_table == NULL) return;

    // 加锁防止多线程同时退出引发竞争
    lock_acquire(&release_thread->file_table->table_lock);
    release_thread->file_table->ref_cnt--;

    if (release_thread->file_table->ref_cnt > 0) {
        // 还有其他线程在共享此表，我不能销毁它，断开指针即可
        lock_release(&release_thread->file_table->table_lock);
        release_thread->file_table = NULL;
        return;
    }

    // 计数归 0，说明当前是最后一个线程，由他物理清除
    lock_release(&release_thread->file_table->table_lock);

    uint8_t local_fd = 0;
    while (local_fd < MAX_FILES_OPEN_PER_PROC) {
        if (release_thread->file_table->fd_table[local_fd].global_fd_idx != -1) {
            // sys_close 内部会自动扣减全局 file_table[].f_count
            sys_close(local_fd); 
        }
        local_fd++;
    }

    // 彻底释放文件表结构
    kfree(release_thread->file_table);

    release_thread->file_table = NULL;
}

// 剥离出来的内存空间安全释放逻辑（配合 sys_wait 中释放页表）
static void release_mm_space(struct task_struct* release_thread) {
    if (release_thread->mm == NULL) return;

    // 获取内存锁防止冲突
    lock_acquire(&release_thread->mm->mm_lock);

    // 检测下溢出
    ASSERT(release_thread->mm->mm_users > release_thread->mm->mm_users - 1);

    release_thread->mm->mm_users--;

    if (release_thread->mm->mm_users > 0) {
        // 还有其他线程在使用这个虚拟内存，不能释放 VMA 和物理页！
        lock_release(&release_thread->mm->mm_lock);
        // 这里不要把 release_thread->mm 设为 NULL
        // 因为后面 sys_wait 还需要读取它的 pgdir 来释放独立页表（如果没有共享的话）
        return;
    }

    lock_release(&release_thread->mm->mm_lock);

    // 计数归 0，说明整个进程所有线程都准备走了，可以安全清理用户空间内存
	// 清理 VMA 链表（释放 kmalloc 申请的 vma 结构体，并 close 关联的 inode）
    clear_vma_list(release_thread);
    release_pg_block(release_thread);
}

static void release_prog_resource(struct task_struct* release_thread) {
    // 减少虚拟内存引用计数/或释放物理页
    release_mm_space(release_thread);

    // 减少文件表引用计数/或关闭文件释放表
    release_files(release_thread);
    
    // 关闭工作目录
    if (release_thread->pwd) {
        inode_close(release_thread->pwd);
        release_thread->pwd = NULL;
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
		pthread->parent_pid = INIT_PID; // init 的 pid 是 1
	}
	return false;
}


pid_t sys_wait(int32_t* status){
	struct task_struct* parent_thread = get_running_task_struct();
	ASSERT(parent_thread->stack_magic == STACK_MAGIC);
	while(1){
		struct dlist_elem* child_elem = dlist_traversal(&thread_all_list,find_hanging_child,(void*)parent_thread->pid);
		if(child_elem!=NULL){
			struct task_struct* child_thread = member_to_entry(struct task_struct,all_list_tag,child_elem);
			if(status!=NULL){
        		*status = child_thread->exit_status;
			}

			// 检查此时的栈顶数据是否合法
    		// struct intr_stack* is = (struct intr_stack*)((uint32_t)get_running_task_struct()->kstack_pages + KERNEL_THREAD_STACK - sizeof(struct intr_stack));
    		// printk("IRET check: CS=%x, EIP=%x, ESP=%x\n", is->cs, is->eip, is->esp);
#ifdef DEBUG_TASK_RECYCLE
			printk("sys_wait: %s recycling %s child status:%d\n",parent_thread->name,child_thread->name,child_thread->exit_status);
#endif

			uint16_t child_pid = child_thread->pid;

			if (child_thread->mm && child_thread->mm->mm_users == 0) {
				release_pg_table(child_thread);
				release_pg_dir(child_thread);
			}
			// release_pg_table(child_thread);
			// release_pg_dir(child_thread);
			thread_exit(child_thread,false);
			// put_str("sys_wait is about to return to userland...\n");
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
	
#ifdef DEBUG_TASK_RECYCLE
			printk("sys_exit: recycling %s status: %d\n",child_thread->name,status);
#endif
	
	if(child_thread->parent_pid == -1){
		PANIC("sys_exit: child_thread->parent_pid is -1\n");
	}

	release_prog_resource(child_thread);

    // 判断当前死亡的线程，是不是其所属虚拟内存空间（进程）里的最后一个活口
    // 如果 mm->mm_users > 0，说明这只是一个普通的 LWP 线程自杀，其他的进程还在运行
	// LWP 退出时，不能直接履行托孤和发送信号的逻辑
	// 因为这会使得其他正在运行的处于同一个线程组中的线程都被托孤给 init！会出现问题！


	// 只有当整个进程的最后一个线程也退出时，才履行通知父进程和过继孤儿的逻辑
    // mm 是在 thread_exit 阶段才释放的，因此此处假如原本有 mm 的话，不会为空
    // 若为空的话，说明是一个内核线程
    if (child_thread->mm == NULL || child_thread->mm->mm_users == 0) {
		dlist_traversal(&thread_all_list,init_adopt_a_child,(void*)child_thread->pid);

		struct task_struct* parent_thread = pid2thread(child_thread->parent_pid);

		// 发送信号，实现异步回收
		// 如果没有信号的话，在先前的实现中，只有父进程调用 wait 了，他才能去回收子进程
		// 但是现在加入了信号，即使父进程没有wait，它只要收到信号了，在信号处理的过程中也会去回收子进程
		send_signal(parent_thread, SIGCHLD);

		if(parent_thread->status==TASK_WAITING){
			thread_unblock(parent_thread);
		}

		// 检查是否有已经变成僵尸的孩子交给了 Init
		// 如果有，唤醒 PID 1
		struct task_struct* init_proc = pid2thread(INIT_PID); // Init 是 PID 1
		if (init_proc) {
			// 扫描全进程表，看看有没有 parent 是 1 且状态是 HANGING 的
			struct dlist_elem* zombie_elem = dlist_traversal(&thread_all_list, find_hanging_child, (void*)INIT_PID);
			if (zombie_elem != NULL) {
				// 发现有过继过来的僵尸，立刻叫醒 Init
				send_signal(init_proc, SIGCHLD);
				if (init_proc->status == TASK_WAITING) {
					thread_unblock(init_proc);
				}
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

#ifdef DEBUG_TASK_RECYCLE
			printk("sys_waitpid: %s recycling %s child status:%d\n",parent_thread->name,child_thread->name,child_thread->exit_status);
#endif

            uint16_t child_pid = child_thread->pid;
			// release_pg_table(child_thread); // 释放页表
			// release_pg_dir(child_thread); // 释放页目录表
			if (child_thread->mm && child_thread->mm->mm_users == 0) {
				release_pg_table(child_thread);
				release_pg_dir(child_thread);
			}
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