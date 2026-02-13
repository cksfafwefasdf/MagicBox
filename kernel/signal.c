#include "signal.h"
#include "thread.h"
#include "wait_exit.h"
#include "interrupt.h"
#include "thread.h"
#include "unistd.h"
#include "errno.h"
#include "debug.h"
#include "stdio-kernel.h"

// 发送信号
void sig_addset(uint32_t* set, int sig) {
    enum intr_status old_status = intr_disable();
    *set |= sigmask(sig);
    intr_set_status(old_status); 
}

// 删除信号
void sig_delset(uint32_t* set, int sig) {
    enum intr_status old_status = intr_disable();
    *set &= ~sigmask(sig);
    intr_set_status(old_status);
}

// 检查信号是否在集合中
bool sig_ismember(uint32_t* set, int sig) {
    enum intr_status old_status = intr_disable();
    return (*set & sigmask(sig)) != 0;
    intr_set_status(old_status); 
}

// 在调用用户自定义的信号处理函数前，先调用该函数
// 保存用户当前上下文环境，然后魔改栈顶的返回地址为 sa_restorer 处指向的函数的地址
// 以便于恢复上下文环境和返回用户之前执行的任务进度
static void setup_frame(int sig, struct sigaction* sa, struct intr_stack* stack,uint32_t blocked) {
    // 计算需要预留的字数（32位字）
    // 备份结构体需要的空间 + 2个位置（sig, sa_restorer）
    // 由于user_esp 是 uint32_t 类型的指针，指针值减一地址会减4
    // 因此我们这里要除以一个 4
    // 由于 sig 和 sa_restorer（地址）本身都是32位数据，因此为他们预留空间直接多减一个2就行
    // 再加一个blocked，所以是减3
    uint32_t stack_size_in_words = DIV_ROUND_UP(sizeof(struct intr_stack), 4) + 3;
    // 这行代码拿到了用户程序刚才在跑的时候的用户态栈的栈顶。
    uint32_t* user_esp = (uint32_t*)stack->esp;
    // 在用户栈开辟空间
    user_esp -= stack_size_in_words;

    // 放置备份数据（紧跟在参数后面）
    // 栈布局：
    // [user_esp + 0]: sa_restorer (返回地址)
    // [user_esp + 1]: sig         (参数)
    // [user_esp + 2]: blocked     (阻塞位图)
    // [user_esp + 3]: intr_stack  (备份的数据从此开始)
    struct intr_stack* save_frame = (struct intr_stack*)(user_esp + 3);


    // 这一步操作将整个intr_stack备份了，包括原先的stack->eip
    // 由于后面我们要魔改stack->eip，因此如果不备份的话我们就没法回到被拦截的用户程序中了
    memcpy(save_frame, stack, sizeof(struct intr_stack));

    // 设置返回地址和参数
    user_esp[0] = (uint32_t)sa->sa_restorer;
    user_esp[1] = sig;
    user_esp[2] = blocked;

    // 修改中断栈帧，准备偷梁换柱
    // 以便于用ret指令跳转到自定义信号处理程序
    stack->esp = (void*)user_esp;
    stack->eip = (void (*)(void))sa->sa_handler;
}

// 从信号量处理函数返回时调用该例程
// 恢复原本的用户上下文，并返回用户程序
// 这虽然是一个系统调用，但是和一般的read和write调用不太一样
// 这个调用是一个“协议”系统调用，它是不会直接开放给用户态程序的
// 它虽然不开给用户程序，但是还是被称为系统调用
// 原因是因为这个函数依然需要通过 int 0x80 中断来调用
// 我们要使用 int 0x80 的原因是因为我们需要进行特权级的切换
// 一般的系统调用业务逻辑都是在内核层中，用户层只接受结果就行
// 但是这个调用特殊在它的业务逻辑是由用户程序提供的，它的业务逻辑工作在用户态
// 因此我们的调用过程不再是 U->K->U 而是 
// U (用户程序原本正常运行) -> K (内核对信号量进行拦截) -> 
// U (内核跳到用户提供的例程中进行业务逻辑) -> K (int 0x80，进入sys_sigreturn恢复用户程序原本的上下文，这个操作用户态下没法进行) ->
// U (回到用户态继续进行后续操作)
// 因此 sys_sigreturn 才这么特殊

// sys_sigreturn 程序较为复杂，里面具有较多的汇编技巧，必须要详细注释一下
// 我们将用户原本在正常运行，然后被信号处理系统拦截下来的程序称为UA，用户的信号处理逻辑称为UB，中间的内核态称为K
// 我们首先需要明确的一点是 UA 和 UB 共享同一个进程的上下文，也就是说它们属于同一个进程，拥有相同的页表、PID 和内存空间。
// 并且共用同一个用户栈, 它们都使用那个从 0xc0000000 向下增长的虚拟内存区域。
// 当 UA 被 K 拦截下来时，K调用 setup_frame 将 UA 的上下文保存在 UA 的用户栈中，并且栈顶指向 sa_restorer
// sa_restorer 的本质就是指向一段代码，这段代码会通过 int 0x80 来调用 sys_sigreturn 进行上下文的恢复
// 当 K 通过 setup_frame 将 UA 的上下文备份完毕，并且将 stack->eip 设置为 (void (*)(void))sa->sa_handler后
// K 会通过 iret 跳转到用户提供的信号处理程序 UB 中（stack->eip 中原本存放的应该是被拦截的用户程序 UA 的返回地址，它是由中断时cpu自动压入的，我们现在将他魔改成用户信号处理函数 UB 了，因此iret会直接回到UB）
// 我们知道，UB和UA共享一个用户栈，因此当UB执行完毕后，由于栈平衡，此时的栈顶指向的是我们刚刚备份的UA的栈顶（x86架构下，esp指向的是栈顶有效数据而不是待插位置）
// 而UA栈顶被我们在setup_frame魔改成sa_restorer了，因此UB执行完毕返回后会直接进入sa_restorer调用 int 0x80 重新陷入内核，进入 sys_sigreturn 函数
// 由于x86下的特权级切换机制中，当 CPU 从 Ring 3（用户态）进入 Ring 0（内核态）时，CPU会自动执行一套硬件级原子操作，将用户态esp和eip等压入到内核栈中
// 因此刚刚UB上下文（准确来说是sa_restorer上下文，sa_restorer和UB也在同一个上下文）中的esp也被压到内核栈了
// 我们在 sys_sigreturn 中只需要将相应的UB的esp取出来，然后稍微进行一点点指针位移操作，就能很快找到之前在setup_frame中备份的UA的上下文环境intr_stack了
// 我们将此时的esp指针指向备份的UA的intr_stack的栈顶，然后调用一条 jmp intr_exit，剩下的就靠 intr_exit 来恢复中断上下文就行了
// 之后 iret 自然就能回到我们最初的 UA 中。
// 由于备份数据存放在用户态，用户程序（或恶意代码）理论上可以在 UB 运行期间篡改这个 intr_stack 备份。
// 因此在 sys_sigreturn 中恢复 CS 和 EFLAGS 时，有必要进行合法性检查，防止用户态通过伪造 CS 获得 Ring 0 权限。但在我们目前来说，就不做这个了

void sys_sigreturn(void) {
    struct task_struct* cur = get_running_task_struct();
    // cur + PG_SIZE 指向用户内核栈的栈顶
    // 我们在内核栈栈顶预留 sizeof(struct intr_stack) 大小的空间
    // 每一个用户程序陷入内核态时，内核调用的栈空间就是用户PCB中预留的那一块内核栈空间
    // 我们在kernel.s中的syscall_handler处，以及将用户的上下文备份了一份在内核栈中了
    struct intr_stack* current_stack = (struct intr_stack*)((uint32_t)cur + PG_SIZE - sizeof(struct intr_stack));


    // 之前 setup_frame 压入用户栈的是
    // [user_esp + 0]: sa_restorer (返回地址)
    // [user_esp + 1]: sig         (参数)
    // [user_esp + 2]: blocked  
    // [user_esp + 3]: intr_stack  (备份的数据从此开始)
    // 由于我们从用户信号处理程序进入sa_restorer时，使用了一个ret指令，因此此时sa_restorer中的栈顶其实指向[user_esp + 1]: sig
    // 因此为了取到我们的intr_stack，我们需要使用user_esp+1，此处 user_esp 是 sa_restorer 的栈顶
    uint32_t* user_esp = (uint32_t*)current_stack->esp;

    // printk("sys_sigreturn: user_esp is %x\n", user_esp);

    // 恢复原本的阻塞位图
    cur->blocked = user_esp[1];

    struct intr_stack* saved_frame = (struct intr_stack*)(user_esp + 2);

    if ((uint32_t)saved_frame < 0x1000) {
        PANIC("sys_sigreturn: saved_frame is invalid!");
    }

    // 恢复现场
    memcpy(current_stack, saved_frame, sizeof(struct intr_stack));

    // 直接跳出 syscall_handler 的逻辑，恢复执行
    // intr_exit 会恢复esp（用户栈）以及eax等寄存器环境
    asm volatile("movl %0, %%esp; jmp intr_exit" :: "g"(current_stack) : "memory");
}


// 向特定进程组发送信号
void kill_pgrp(pid_t pgrp, int sig) {
    // 遍历全局任务列表必须加锁或关中断，防止链表结构在遍历时被改变
    enum intr_status old_status = intr_disable();

    struct dlist_elem* node = thread_all_list.head.next;
    while (node != &thread_all_list.tail) {
        struct task_struct* task = member_to_entry(struct task_struct, all_list_tag, node);
        
        // 匹配前台进程组且是非内核线程
        if (task->pgdir != NULL && task->pgrp == pgrp) {
            sig_addset(&task->signal, sig);
        }
        node = node->next;
    }

    intr_set_status(old_status);
}

void do_signal(struct intr_stack* stack) {
    struct task_struct* cur = get_running_task_struct();

    // SIGSTOP 和 SIGKILL 是不可屏蔽信号，因此每一次运行前先重置它们
    // 防止用户将其屏蔽
    cur->blocked &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP)); 

	// 检查待处理的信号量，并且讲屏蔽信号量的位强制置 0 
    uint32_t pending = cur->signal & ~cur->blocked;
    // 由于在用户态下，每收到一个时钟中断，我们都会进一遍这个函数
    // 因此前面这个快速返回的逻辑很重要，会极大的影响系统的运行效率
    // 如果没有这几行快速返回的代码的话，系统的速度会变得很慢
    if (!pending) return;

    enum intr_status old_status = intr_disable();
    
    pending = cur->signal & ~cur->blocked;

    // 再次检查，防止在关中断前的最后一秒位图被更改
    if (!pending) {
        goto end;
    }

    int sig = 0,i = 1;
    // 查找位图，低位的信号优先
    for (i = 1; i <= 31; i++) {
        if (pending & sigmask(i)) {
            sig = i;
            break;
        }
    }

    if (sig == 0) {
        goto end;
    } 

    // 执行该信号了，因此清除信号位
    // 如果 sa_handler 为 SIG_IGN 的话，那么后面两个if都不会执行
    // 真正执行的操作就只是将位图清空而已
    cur->signal &= ~sigmask(sig);
    struct sigaction* sa = &cur->sigactions[sig - 1];

    // 如果是忽略，直接清除位图走人（
    if (sa->sa_handler == SIG_IGN) {
        goto end; 
    }

    if (sa->sa_handler == SIG_DFL) {
        switch (sig) {
            case SIGCHLD: // 子进程已停止或终止
                // 默认忽略，什么都不做，因为一般父进程会负责收尾
                goto end;
            case SIGALRM: // 该信号会在用户调用alarm系统调用所设置的延迟时间到后产生。该信号常用于判别系统调用超时。 
            case SIGTERM: // 用于和善地要求一个程序终止。它是kill的默认信号。与SIGKILL不同，该信号能被捕获，这样就能在退出运行前做清理工作。 
            case SIGKILL: // 程序被终止。该信号不能被捕获或者被忽略。想立刻终止一个进程，就发送信号9。程序没有任何机会做清理工作。
            case SIGINT: // 来自键盘的中断。通常终端驱动程序会将其与^C绑定。
            case SIGPIPE: // 当程序向一个套接字或管道写时由于没有读者而产生该信s号。
                // printk("alrm!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
                sys_exit(-sig); // 终止进程
                break;
            case SIGSEGV: // 当程序引用无效的内存时会产生此信号。比如：寻址没有映射的内存；寻址未许可的内存。 
            case SIGILL: // 程序出错或者执行了一条非法操作指令。
                // 这些属于需要 "Core Dump" 的信号
                // 现阶段只打印栈回溯 + sys_exit
                printk("\nTriggered %s, Backtrace:\n", sig == SIGSEGV ? "SIGSEGV" : "Exception");
                print_stacktrace(); 
                printk("\n");
                sys_exit(-sig);
                break;
            default:
                goto end;
        }
    }

    // 只有 handler 存在且 restorer 存在时才进行栈重构
    // setup_frame 会将返回地址直接改成sa_handler，因此setup_frame函数不会返回到后续逻辑了
    if (sa->sa_handler != SIG_IGN && sa->sa_handler != SIG_DFL) {
        // 安全检查：如果 restorer 为空，说明用户没设置，或者没用 Libc 封装
        // 如果你没有全局默认 restorer，这里可能需要强制 sys_exit 或者打印警告
        if (sa->sa_restorer == NULL) {
             printk("Error: Signal %d handler has no restorer!\n", sig);
             PANIC("sa->sa_restorer == NULL !!!");
             // sys_exit(-sig);
             // return;
        }
        // 备份原本的屏蔽位图，以便在sys_sigreturn中恢复
        uint32_t old_blocked = cur->blocked;

        // 设置新的屏蔽位：屏蔽自身 + sa_mask
        cur->blocked |= (sigmask(sig) | sa->sa_mask);
        // 防止用户屏蔽 SIGSTOP 和 SIGKILL
        cur->blocked &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP)); 
        setup_frame(sig, sa, stack, old_blocked);
    }

end:
    intr_set_status(old_status);
}

// linux 中的原型是 void (*signal(int sig, void (*fn)(int)))(int);
// 看起来很复杂，其实就和我们现在的定义是一样的，接受一个新的函数，将原来旧的handler函数返回
// 以便于恢复现场
void* sys_signal(int sig, void* handler) {
    struct task_struct* cur = get_running_task_struct();
    
    // 校验信号范围 (1-31)
    if (sig < 1 || sig > SIG_NR-1) return (void*)-1;
    
    // 保存旧的处理函数地址
    void* old_handler = cur->sigactions[sig - 1].sa_handler;
    
    // 设置新的处理函数 (SIG_IGN, SIG_DFL 或用户函数地址)
    cur->sigactions[sig - 1].sa_handler = handler;
    
    // 注意，Linux 的 signal 会重置 sa_mask 和 sa_flags
    cur->sigactions[sig - 1].sa_mask = 0;
    cur->sigactions[sig - 1].sa_flags = 0; 

    return old_handler;
}

// sys_signal 的进化版
int sys_sigaction(int sig, const struct sigaction* act, struct sigaction* oact) {
    if (sig < 1 || sig > 32) return -EINVAL;

    struct task_struct* cur = get_running_task_struct();

    // 如果用户想保存旧的设置 (oact)
    if (oact != NULL) {
        *oact = cur->sigactions[sig - 1];
    }

    if (act != NULL) {
        cur->sigactions[sig - 1] = *act;
    }

    return 0;
}

// 向目标线程发送指定信号
void send_signal(struct task_struct* target, int sig) {
    if (target == NULL || sig > SIG_NR-1 || sig < 1) return;

    // 修改目标进程的信号位图
    enum intr_status old_status = intr_disable();
    target->signal |= sigmask(sig);
    intr_set_status(old_status);

    // 唤醒目标进程
    // 如果目标进程正在不可中断的睡眠中（比如等待 IO，此时为 block），我们通常不唤醒。
    // 但如果它正在 TASK_WAITING 状态，此时它正在等信号，必须拉它一把
    // 这样它才能回到 do_signal 的检查点。
    if (target->status == TASK_WAITING) {
        // 如果它在等待信号，就让它进入就绪队列
        thread_unblock(target); 
    }
}

int sys_kill(pid_t pid, int sig) {
    struct task_struct* target = pid2thread(pid);
    if (target == NULL) return -1;
    
    send_signal(target, sig);
    return 0;
}

int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset){
    struct task_struct* cur = get_running_task_struct();
    
    // 如果用户想保存旧的位图，先备份
    if (oldset != NULL) {
        *oldset = cur->blocked;
    }

    // 如果 set 为 NULL，说明用户只想读取 oldset，不想修改
    if (set == NULL) {
        return 0;
    }

    uint32_t new_set = *set;

    // 根据 how 执行原子操作
    enum intr_status old_status = intr_disable();
    
    switch (how) {
        case SIG_BLOCK:
            cur->blocked |= new_set;
            break;
        case SIG_UNBLOCK:
            cur->blocked &= ~new_set;
            break;
        case SIG_SETMASK:
            cur->blocked = new_set;
            break;
        default:
            intr_set_status(old_status);
            return -EINVAL; // 无效的操作类型
    }

    // 强制不能阻塞 SIGKILL 和 SIGSTOP
    // 这是 POSIX 标准要求的，保证进程永远能被管理员强制杀死或暂停
    cur->blocked &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));

    intr_set_status(old_status);
    return 0;
}

// 获取已产生但被阻塞的信号
int sys_sigpending(uint32_t* set) {
    struct task_struct* cur = get_running_task_struct();
    
    // 逻辑很简单，正在挂起的信号 & 被屏蔽的信号
    // 如果一个信号 pending 了但没被 blocked，它通常已经进 do_signal 处理了
    // 所以 pending & blocked 就是那些“在后台静默排队”的信号
    if (set != NULL) {
        *set = (cur->signal & cur->blocked);
        return 0;
    }
    return -EFAULT;
}