#include "init.h"
#include "interrupt.h"
#include "print.h"
#include "timer.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "keyboard.h"
#include "tss.h"
#include "syscall-init.h"
#include "ide.h"
#include "fs.h"


void init_all(void){
    put_str("init all start...\n");
    intr_init();
    mem_init();
    thread_environment_init();
    timer_init();
    console_init();
    keyboard_init();
    tss_init();
    syscall_init();
    intr_enable(); // ide_init will use the interrupt
    ide_init();
    filesys_init();
}