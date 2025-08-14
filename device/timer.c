#include "timer.h"
#include "io.h"
#include "print.h"
#include "thread.h"
#include "debug.h"
#include "interrupt.h"

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

static void intr_handler_timer(void){
    struct task_struct* cur_thread = get_running_task_struct();
    //put_str(cur_thread->name);put_str(" timer!!!\n");put_int(cur_thread->ticks);
    // check if stack overflouw
    ASSERT(cur_thread->stack_magic == 0x20030607);

    cur_thread->elapsed_ticks++;
    ticks++;

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