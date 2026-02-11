#include "timer.h"
#include "io.h"
#include "print.h"
#include "thread.h"
#include "debug.h"
#include "interrupt.h"
#include "dlist.h"
#include "stdbool.h"
#include "signal.h"
#include "errno.h"

#define IRQ0_FREQUENCY 100 //intr freq is 100 times/s
#define INPUT_FREQUENCY 1193180
#define COUNT0_INIT_COUNT_VALUE INPUT_FREQUENCY/IRQ0_FREQUENCY
#define COUNT0_PORT 0x40 
#define COUNT0_NO 0 // select counter0
#define COUNT0_MODE 2 // Rate Generator mode,software start
#define READ_WRITE_LATCH_MODE 3 // read/write low 8 bit at first,then do it again for high 8 bit
#define PIT_CONTROL_PORT 0x43 
#define IS_BCD 0 // BCD or binary 

#define mil_seconds_per_intr (1000/IRQ0_FREQUENCY)

// 在时钟中断频率为 100 次每秒的情况下，一个 32 位的uint32_t tick 数据发生溢出环回大约要497天
// 也就是说系统要连续运行 497 天这个时钟才会溢出环回，因此问题不大
// 即使改成 int 类型，也得要 248 天左右
// 通常来说，对于ticks数，使用uint类型更好，因为uint类型即使发生溢出环回，行为也比较简单
uint32_t ticks; // total ticks since the interrupt was enabled, it just like a system time 


static void freq_set(
        uint8_t counter_port,
        uint8_t counter_no,
        uint8_t rwl,
        uint8_t counter_mode,
        uint16_t counter_init_value,
        uint8_t is_bcd
    ){
    uint8_t control_word = counter_no<<6|rwl<<4|counter_mode<<1|is_bcd;
    outb(PIT_CONTROL_PORT,control_word);
    outb(counter_port,(uint8_t)counter_init_value); // write low 8bits
    outb(counter_port,(uint8_t)(counter_init_value>>8)); //write high 8bits
}

// 用于 dlist_traversal 的回调函数
// 返回 false 表示继续遍历，不会中途停止
static bool check_alarm_and_signal(struct dlist_elem* pelem, void* arg) {
    uint32_t current_ticks = *((uint32_t*)arg);
    struct task_struct* pthread = member_to_entry(struct task_struct, all_list_tag, pelem);

    // 检查闹钟是否到期
    if (pthread->alarm > 0 && pthread->alarm <= current_ticks) {
        sig_addset(&pthread->signal, SIGALRM);
        pthread->alarm = 0; // 闹钟重置

        // 软实时的核心, 唤醒正在睡眠的进程
        // 如果进程在阻塞态，因为闹钟响了，必须把它拉回就绪队列
        if (pthread->status == TASK_BLOCKED || pthread->status == TASK_WAITING || pthread->status == TASK_HANGING) {
            thread_unblock(pthread);
        }
    }
    
    return false; // 继续遍历下一个进程，总是返回false以便于遍历整个链表
}

static void intr_handler_timer(void){
    struct task_struct* cur_thread = get_running_task_struct();
    //put_str(cur_thread->name);put_str(" timer!!!\n");put_int(cur_thread->ticks);
    // check if stack overflouw
    ASSERT(cur_thread->stack_magic == STACK_MAGIC);

    cur_thread->elapsed_ticks++;
    ticks++;

    // 由于我们每一个时钟中断才检查一次所有进程的闹钟
    // 并且检查过程还会再花费一些时间
    // 一因此如果定时 5s，我们真正可能得花5s上下多浮动若干ms才能把一个进程唤醒
    // 因此我们的系统是软实时的
    dlist_traversal(&thread_all_list, check_alarm_and_signal, &ticks);

    if(cur_thread->ticks==0){
        schedule();
    }else{
        cur_thread->ticks--;
    }
}

void timer_init(void){
    put_str("timer_init start\n");
    freq_set(COUNT0_PORT,COUNT0_NO,READ_WRITE_LATCH_MODE,COUNT0_MODE,COUNT0_INIT_COUNT_VALUE,IS_BCD);
    // 0x20 is IRQ0
    register_handler(0x20,intr_handler_timer);
    put_str("timer_init done");
}


// sleep is measured in ticks; all time-based sleeps are converted to this tick format.
static void ticks_to_sleep(uint32_t sleep_ticks){
    uint32_t start_tick = ticks;

    while(ticks-start_tick<sleep_ticks){
        thread_yield();
    }
}

// sleep is measured in mil-second
void mtime_sleep(uint32_t m_seconds){
    uint32_t sleep_ticks = DIV_ROUND_UP(m_seconds,mil_seconds_per_intr);
    ASSERT(sleep_ticks>0);
    ticks_to_sleep(sleep_ticks);
}

int sys_pause(void) {
    // 将自己设为阻塞状态，等待信号唤醒
    // 进程不再处于 ready_list 中
    thread_block(TASK_BLOCKED); 
    
    // 当进程被唤醒（比如被你的 check_alarm_and_signal 唤醒）
    // schedule() 返回后会执行到这里
    
    // 根据 POSIX 标准，pause 总是返回 -1，并设置 errno 为 EINTR
    // 因为它只有在捕捉到信号并从处理函数返回后才会“结束”
    return -EINTR; 
}

uint32_t sys_alarm(uint32_t seconds) {
    enum intr_status old_status = intr_disable();
    struct task_struct* cur = get_running_task_struct();
    uint32_t old_alarm = cur->alarm;
    uint32_t remaining = 0;

    // 如果之前已经有闹钟，计算剩余秒数返回
    if (old_alarm > 0) {
        if (old_alarm > ticks) {
            remaining = (old_alarm - ticks) / IRQ0_FREQUENCY;
        } else {
            remaining = 0; // 已经到期但信号可能还在 pending
        }
    }

    if (seconds > 0) {
        cur->alarm = ticks + (seconds * IRQ0_FREQUENCY);
    } else {
        cur->alarm = 0; // seconds 为 0 表示取消闹钟
    }
    intr_set_status(old_status);

    return remaining;
}