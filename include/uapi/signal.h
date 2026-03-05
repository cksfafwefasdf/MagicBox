#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H
#include "stdint.h"
#include "stdbool.h"
#include "unistd.h"

struct task_struct;
struct intr_stack;

#define SIG_NR 32

// IGN 和 DFL 相当于是两个返回值为 void 类型，参数为一个 int 类型的函数
// 这两个函数的地址分别是 0 和 1
// 或者说我们将 0 和 1 这两个数强转成了 (void (*)(int)) 类型
// 这是一种状态压缩，因为这相当于是将 0 和 1 传入到 signal 中，正常的用户函数的地址不可能是这样的地址
// 因此一旦 do_signal 识别到 0 和 1 那么就可以执行特殊的逻辑，而不用真的去执行传入的函数
// signal 函数只是个搬运工，只负责无脑复制函数指针，真正区分这些指针和状态的函数是 do_signal
#define SIG_DFL  ((void (*)(int))0) // Signal Default，调用默认行为，例如对于SIGINT (Ctrl+C) 直接杀死进程
#define SIG_IGN  ((void (*)(int))1) // Signal Ignore 仅仅清理信号位图 cur->signal &= ~mask

#define SIGHUP    1
#define SIGINT   2
#define SIGQUIT   3
#define SIGILL   4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD  17
#define SIGSTOP   19
#define SIGTSTP   20

// sys_sigprocmask 的 how 常量
#define SIG_BLOCK   0 // 把一组新信号加入当前屏蔽位图（blocked |= set）。
#define SIG_UNBLOCK 1 // 把一组信号从当前屏蔽位图移除（blocked &= ~set）。
#define SIG_SETMASK 2 // 直接用新的位图替换旧的（blocked = set）。

// SIGCHLD SIGTERM SIGALRM SIGSEGV SIGKILL SIGINT SIGILL SIGPIPE


// 信号 1-32 映射到信号位图的 0-31 位
#define sigmask(sig) (1 << ((sig) - 1))

// 定义信号的默认行为类型（内部使用）
enum sig_action_type {
    TERMINATE, // 终止进程
    CORE_DUMP, // 终止并打印/产生 Dump（先实现为打印回溯）
    IGNORE, // 忽略
    STOP, // 停止进程
    CONTINUE // 继续运行
};

extern void sig_addset(uint32_t* set, int sig);
extern void sig_delset(uint32_t* set, int sig);
extern bool sig_ismember(uint32_t* set, int sig);
extern void do_signal(struct intr_stack* stack);
extern void kill_pgrp(pid_t pgrp, int sig);
extern void send_signal(struct task_struct* target, int sig);
extern void* sys_signal(int sig, void* handler);
extern void sys_sigreturn(void);
extern int sys_sigaction(int sig, const struct sigaction* act, struct sigaction* oact);
extern int sys_kill(pid_t pid, int sig);
extern int sys_sigpending(uint32_t* set);
extern int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset);

#endif