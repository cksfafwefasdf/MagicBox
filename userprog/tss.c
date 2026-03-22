#include <stdint.h>
#include <global.h>
#include <print.h>
#include <string.h>
#include <thread.h>
#include <tss.h>
#include <init.h>

// each CPU only have one TSS
struct tss tss;

extern struct task_struct*  main_thread;

void update_tss_esp(struct task_struct* pthread){
	// top of the kernel stack
	// tss.esp0 = (uint32_t*) ((uint32_t)pthread + PG_SIZE);
	tss.esp0 = (uint32_t*)((uint32_t)pthread->kstack_pages + KERNEL_THREAD_STACK);
}

static struct gdt_desc make_gdt_desc(uint32_t* desc_addr,uint32_t limit,uint8_t attr_low,uint8_t attr_high){
	uint32_t desc_base = (uint32_t) desc_addr;
	struct gdt_desc desc;
	desc.limit_low_word = limit&0x0000ffff;
	desc.base_low_word = desc_base&0x0000ffff;
	desc.base_mid_byte = ((desc_base&0x00ff0000)>>16);
	desc.attr_low_byte = (uint8_t) (attr_low);
	desc.limit_high_attr_high = (((limit&0x000f0000)>>16)+(uint8_t)(attr_high));
	desc.base_high_byte = desc_base>>24;
	return desc;
}

// create tss in gdt
void tss_init(void){
	put_str("tss_init start\n");
	uint16_t tss_size = (uint16_t)sizeof(tss);
	memset(&tss,0,tss_size);
	tss.ss0 = SELECTOR_K_STACK; // ss0 will not change
	tss.io_base = tss_size;

	// 初始时，cur_task 就是 main_thread
	tss.cur_task = main_thread;
	
	// tss desc is 4th desc, each desc is 8bytes 
	// gdt in 0x900, so tss desc in 0x900+4*8bytes=0x900+0x20
	// my gdt in 0x903, so gdt_base is 0x903
	uint32_t gdt_base = GDT_BASE;
	*((struct gdt_desc*)(gdt_base+4*8)) = make_gdt_desc((uint32_t*)&tss,tss_size-1,TSS_ATTR_LOW,TSS_ATTR_HIGH);
	
	// 5th desc is user_code_desc
	*((struct gdt_desc*)(gdt_base+5*8)) = make_gdt_desc((uint32_t*)0,0xfffff,GDT_CODE_ATTR_LOW_DPL3,GDT_ATTR_HIGH);
	// 6th is user_data_desc
	*((struct gdt_desc*)(gdt_base+6*8)) = make_gdt_desc((uint32_t*)0,0xfffff,GDT_DATA_ATTR_LOW_DPL3,GDT_ATTR_HIGH);
	
	// data in gdtr which is 48bits
	// low 16bits is limit,high 32bits is base addr
	// each desc is 8bytes, 7 desc is 7*8bytes,count from 0, so result is 7*8-1
	// so 7*8-1 is limit
	uint64_t gdt_operand = (((uint64_t)gdt_base<<16))|((8*7-1));
	
	asm volatile ("lgdt %0"::"m"(gdt_operand));
	asm volatile ("ltr %w0"::"r"(SELECTOR_TSS));
	put_str("tss_init done\n");
}