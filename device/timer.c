#include <timer.h>
#include <io.h>
#include <vgacon.h>
#include <thread.h>
#include <debug.h>
#include <interrupt.h>
#include <dlist.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>

// 在时钟中断频率为 100 次每秒的情况下，一个 32 位的uint32_t tick 数据发生溢出环回大约要497天
// 也就是说系统要连续运行 497 天这个时钟才会溢出环回，因此问题不大
// 即使改成 int 类型，也得要 248 天左右
// 通常来说，对于ticks数，使用uint类型更好，因为uint类型即使发生溢出环回，行为也比较简单
uint32_t ticks; // total ticks since the interrupt was enabled, it just like a system time

// 如果 sleep 的时钟早于 alarm，alarm 仍然会有效果
// 因为他是个异步的行为，如果 sleep 3 秒，然后 alarm 5 秒
// 程序会在醒来 2 秒后被销毁（alarm的默认行为是销毁程序）
// 如果 alarm 3 秒 sleep 5 秒，程序直接会在 3 秒时被销毁
struct dlist timer_list; // 存放与 wait_until 相关的进程
struct dlist alarm_list; // 存放与 alarm 相关的进程


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

// 这个函数的调用频率不高，多写了一些判断语句应该没关系
// 用三元表达式是为了让编译器尽量生产条件转移语句 cmov，以免出现控制冒险
static void list_insert_ascend(struct task_struct* pthread, struct dlist* plist) {
    enum intr_status old_status = intr_disable(); // 必须关中断，否则打印时链表可能被中断修改

    ASSERT((plist==&alarm_list) || (plist==&timer_list));
    
    struct dlist_elem* tag = (plist == &alarm_list)?(&pthread->alarm_tag):(&pthread->timer_tag);

    // 防止重复插入，重复插入会导致非常严重的不一致问题
    ASSERT(!dlist_is_linked(tag));

    uint32_t target = (plist == &alarm_list)?(pthread->alarm):(pthread->wait_until);
    
    struct dlist_elem* pelem = (plist == &alarm_list)?(alarm_list.head.next):(timer_list.head.next);
    
    ASSERT(pelem!=NULL);

    // 找到第一个比自己晚的节点，插在它的前面
    while (pelem != &plist->tail) {
        struct task_struct* temp;
        temp = (plist == &alarm_list)?
                (member_to_entry(struct task_struct, alarm_tag, pelem)):
                (member_to_entry(struct task_struct, timer_tag, pelem));


        uint32_t temp_target = (plist == &alarm_list) ? temp->alarm : temp->wait_until;
        if (temp_target > target) break;

        pelem = pelem->next;
    }

    ASSERT(pelem!=NULL);
    
    dlist_insert_front(pelem, tag);

    intr_set_status(old_status);
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
    // 检查定时器队列
    // 由于是升序排列，只要队头没到期，后面的一定没到期
    // 由于这里面可能会涉及到节点的移除，并且该段代码执行的频率非常高
    // 因此我们不复用 dlist_traversal，因为他会增加大量的函数调用上下文
    // 这段代码移除一个节点的操作复杂度是 O(1)，但是可能会移除 n 个节点，因此总的大概还是 O(n)
    // 用 while 可以保证所有到期的节点都被移除
    while (!dlist_empty(&timer_list)) {
        struct dlist_elem* pelem = timer_list.head.next;
        struct task_struct* pthread = member_to_entry(struct task_struct, timer_tag, pelem);

        bool timer_done = (pthread->wait_until > 0 && pthread->wait_until <= ticks);

        if (!timer_done) {
            break; // 队头都没到时间，后面的节点的结束时间更靠后，直接结束检查
        }

        // 到期了，从定时器队列移除
        if (dlist_is_linked(&pthread->timer_tag)) {
            dlist_remove(&pthread->timer_tag);
        }

        pthread->wait_until = 0;

        // 唤醒处于阻塞态的进程
        if (pthread->status == TASK_BLOCKED || pthread->status == TASK_WAITING || pthread->status == TASK_HANGING) {
            thread_unblock(pthread);
        }
    }

    while (!dlist_empty(&alarm_list)) {
        struct dlist_elem* pelem = alarm_list.head.next;
        struct task_struct* pthread = member_to_entry(struct task_struct, alarm_tag, pelem);

        ASSERT(pthread->status != TASK_DIED);

        bool alarm_done = (pthread->alarm > 0 && pthread->alarm <= ticks);

        if (!alarm_done) {
            break; // 队头都没到时间，后面的节点的结束时间更靠后，直接结束检查
        }

        // 到期了，从定时器队列移除
        if (dlist_is_linked(&pthread->alarm_tag)) {
            dlist_remove(&pthread->alarm_tag);
        }
        
        sig_addset(&pthread->signal, SIGALRM);
        pthread->alarm = 0;

        // 唤醒处于阻塞态的进程
        if (pthread->status == TASK_BLOCKED || pthread->status == TASK_WAITING || pthread->status == TASK_HANGING) {
            thread_unblock(pthread);
        }
    }

    // 时间片检查与调度
    if (cur_thread->ticks == 0) {
        schedule();
    } else {
        cur_thread->ticks--;
    }
}

void timer_init(void){
    put_str("timer_init start\n");
    freq_set(COUNT0_PORT,COUNT0_NO,READ_WRITE_LATCH_MODE,COUNT0_MODE,COUNT0_INIT_COUNT_VALUE,IS_BCD);
    // 0x20 is IRQ0
    register_handler(0x20,intr_handler_timer);
    dlist_init(&timer_list);
    dlist_init(&alarm_list);
    put_str("timer_init done");
}

// sleep is measured in mil-second
int32_t sys_milsleep(uint32_t mil_seconds) {
    uint32_t sleep_ticks = DIV_ROUND_UP(mil_seconds, mil_seconds_per_intr);
    if (sleep_ticks == 0) return 0;

    struct task_struct* cur = get_running_task_struct();
    enum intr_status old_status = intr_disable();

    uint32_t expire_ticks = ticks + sleep_ticks;
    cur->wait_until = expire_ticks;
    
    // 放入排序后的时间队列
    list_insert_ascend(cur,&timer_list);
    
    // 阻塞自己，等待时钟中断将其拉回就绪队列
    thread_block(TASK_BLOCKED); 

    // 醒来后的处理
    uint32_t remaining_ms = 0;

    // 兜底逻辑，醒来后检查是不是因为“还没到期”就被信号提前唤醒
    // 如果是正常时间到了被唤醒的，tag 会在时钟中断程序中被摘除
    // 但是如果是被信号量或者其他东西强制唤醒的，不会走到时钟中断里面的那个逻辑
    if (dlist_is_linked(&cur->timer_tag)) {
        if (expire_ticks > ticks) {
            remaining_ms = (expire_ticks - ticks) * mil_seconds_per_intr;
        }
        dlist_remove(&cur->timer_tag);
        cur->timer_tag.prev = cur->timer_tag.next = NULL;
    }
    
    intr_set_status(old_status);
    // 如果被中断返回剩余时间，否则返回 0
    return remaining_ms;
}

int sys_pause(void) {
    // 将自己设为阻塞状态，等待信号唤醒
    // 进程不再处于 ready_list 中
    thread_block(TASK_BLOCKED); 
    
    // 当进程被唤醒
    // schedule() 返回后会执行到这里
    
    // 根据 POSIX 标准，pause 总是返回 -1，并设置 errno 为 EINTR
    // 因为它只有在捕捉到信号并从处理函数返回后才会“结束”
    return -EINTR; 
}

int32_t sys_alarm(uint32_t seconds) {
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

    // 如果已经在队列里，先摘下来
    if (dlist_is_linked(&cur->alarm_tag)) {
        dlist_remove(&cur->alarm_tag);
    }

    if (seconds > 0) {
        cur->alarm = ticks + (seconds * IRQ0_FREQUENCY);
        list_insert_ascend(cur,&alarm_list); // 重新插入排序后的位置
    } else {
        cur->alarm = 0; // seconds 为 0 表示取消闹钟
    }
    intr_set_status(old_status);

    return remaining;
}