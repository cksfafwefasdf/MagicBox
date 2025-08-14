#include "interrupt.h"
#include "stdint.h"
#include "global.h"
#include "print.h"
#include "io.h"

#define IDT_DESC_CNT 0x81

#define PIC_M_CTRL 0x20 // master-control-port => ICW1，OCW2, OCW3
#define PIC_M_DATA 0x21 // master-data-port => ICW2 ICW3 ICW4  OCW1
#define PIC_S_CTRL 0xa0 // slave-control-port => ICW1，OCW2, OCW3
#define PIC_S_DATA 0xa1 // slave-data-port => ICW2 ICW3 ICW4  OCW1

#define EFLAGS_IF 0x00000200

#define SCREEN_POS(row,col) (row*NUM_FULL_LINE_CH+col) 

#define GET_EFLAGS(EFLAGS_VAR) asm volatile ("pushfl;popl %0":"=g"(EFLAGS_VAR));

struct intr_gate_desc{
    uint16_t func_offset_low_word;
    uint16_t selector;
    uint8_t dcount; // low 8bits of the desc's high 32bit,this part is 0
    uint8_t attribute;
    uint16_t func_offset_high_word;
};

static void make_idt_desc(struct intr_gate_desc* p_gdesc,uint8_t attr,intr_handler_addr addr);
static struct intr_gate_desc idt[IDT_DESC_CNT];
static void pic_init(void);

char* intr_name[IDT_DESC_CNT]; //a char* array equals to a string array
intr_handler_addr idt_ISR[IDT_DESC_CNT];

extern intr_handler_addr intr_entry_table[IDT_DESC_CNT];
extern uint32_t syscall_handler(void);

static void make_idt_desc(struct intr_gate_desc* p_gdesc,uint8_t attr,intr_handler_addr addr){
    p_gdesc->attribute=attr;
    p_gdesc->dcount=0;
    p_gdesc->selector=SELECTOR_K_CODE;
    p_gdesc->func_offset_low_word=(uint32_t)addr&0x0000ffff;
    p_gdesc->func_offset_high_word=(((uint32_t)addr&0xffff0000)>>16);
}

static void idt_item_init(void){
    put_str("idt_item_init start\n");
    // lastindex is 0x80
    int i,lastindex = IDT_DESC_CNT-1;
    for(i=0;i<IDT_DESC_CNT;i++){
        make_idt_desc(&idt[i],IDT_DESC_ATTR_DPL0,intr_entry_table[i]);
    }
    // process 0x80 specially 
    // because syscall is called by user
    // so DPL is 3
    make_idt_desc(&idt[lastindex],IDT_DESC_ATTR_DPL3,syscall_handler);
    put_str("idt_item_init done\n");
}

static void pic_init(void){
    // master
    outb(PIC_M_CTRL,0x11);
    outb(PIC_M_DATA,0x20);
    outb(PIC_M_DATA,0x04);
    outb(PIC_M_DATA,0x01);

    //slave
    outb(PIC_S_CTRL,0x11);
    outb(PIC_S_DATA,0x28);
    outb(PIC_S_DATA,0x02);
    outb(PIC_S_DATA,0x01);

    // allow master-IR0 only
    //outb(PIC_M_DATA,0xfe);
    //outb(PIC_S_DATA,0xff);

    // allow IRQ0(timer),IRQ1(keyboard) ,IRQ2(slave 8259A),IRQ14(ata-1)
    // IMR for master 
    outb(PIC_M_DATA,0xf8);
    // IMR(intr mask) for slave
    outb(PIC_S_DATA,0xbf);

    put_str("init pic done\n");
}

// default intr handler
static void intr_handler_general(uint8_t intr_vec){
    if(intr_vec==0x27||intr_vec==0x2f){
        // IRQ7 IRQ15 will cause spurious interrupt.
        // we dont need to handle it
        return ;
    }

    // clear space to print exception infos
    set_cursor(SCREEN_POS(0,0));
    int cursor_pos = 0;
    while(cursor_pos<SCREEN_POS(4,0)){
        put_char(' ');
        cursor_pos++;
    }

    set_cursor(SCREEN_POS(0,0));
    put_str("!!!!!!!!!! exception message begin !!!!!!!!!!\n");
    set_cursor(SCREEN_POS(1,8));
    put_str(intr_name[intr_vec]);
    // if it is pagefault
    if(intr_vec==14){
        int page_fault_vaddr = 0;
        asm("movl %%cr2,%0":"=r"(page_fault_vaddr));
        put_str("\n\t\tpage fault addr is ");put_int(page_fault_vaddr);
    }
    put_str("\n!!!!!!!!!! exception message end !!!!!!!!!!\n");
    while (1);
    
}

static void exception_init(void){
    int idx = 0;
    for(idx=0;idx<IDT_DESC_CNT;idx++){
        idt_ISR[idx] = intr_handler_general; // function-ptr
        intr_name[idx] = "unknown"; // init it unknown at first
    }

    intr_name[0] = "#DE Divide Error"; 
    intr_name[1] = "#DB Debug Exception"; 
	intr_name[2] = "NMI Interrupt";
	intr_name[3] = "#BP Breakpoint Exception"; //即 int3
 	intr_name[4] = "#OF Overflow Exception"; //即 into
	intr_name[5] = "#BR BOUND Range Exceeded Exception"; 
	intr_name[6] = "#UD Invalid Opcode Exception"; 
    intr_name[7] = "#NM Device Not Available Exception"; 
	intr_name[8] = "#DF Double Fault Exception"; 
	intr_name[9] = "Coprocessor Segment Overrun"; 
	intr_name[10] = "#TS invalid TSS Exception"; 
	intr_name[11] = "#NP Segment Not Present"; 
	intr_name[12] = "#SS Stack Fault Exception"; 
	intr_name[13] = "#GP General Protection Exception"; 
	intr_name[14] =	"#PF Page-Fault Exception"; 
	// intr_name[15］第 15 项是 intel 保留项，未使用
	intr_name[16] = "#MF x87 FPU Floating-Point Error"; 
	intr_name[17] = "#AC Alignment Check Exception"; 
	intr_name[18] = "#MC Machine-Check Exception"; 
	intr_name[19] = "#XF SIMD Floating-Point Exception";     

}


void intr_init(void){
    put_str("idt items init\n");
    idt_item_init();
    put_str("idt items init done\n");
    exception_init();
    pic_init();

    //load idt
    uint64_t idtr_data = (((uint64_t)(uint32_t)idt)<<16)|(sizeof(idt)-1); 
    asm volatile ("lidt %0"::"m"(idtr_data));
    put_str("intr_init done\n");
}

void register_handler(uint8_t vec_no,intr_handler_addr function){
    idt_ISR[vec_no] = function;
}

enum intr_status intr_get_status(void){
    uint32_t eflags_var=0;
    GET_EFLAGS(eflags_var);
    return (eflags_var&EFLAGS_IF)?INTR_ON:INTR_OFF;
}

enum intr_status intr_enable(void){
    enum intr_status old_status = intr_get_status();
    if(INTR_OFF==old_status) asm volatile ("sti":::"memory");
    return old_status;
}

enum intr_status intr_disable(void){
    enum intr_status old_status = intr_get_status();
    if(INTR_ON==old_status) asm volatile ("cli":::"memory");
    return old_status;
}

enum intr_status intr_set_status(enum intr_status status){
    return (status==INTR_ON?intr_enable():intr_disable());
}
