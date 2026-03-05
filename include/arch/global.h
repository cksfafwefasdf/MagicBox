#ifndef __KERNEL_GLOBAL_H
#define __KERNEL_GLOBAL_H
#include "stdint.h"

#define RPL0 0
#define RPL1 1
#define RPL2 2
#define RPL3 3

#define TI_GDT 0
#define TI_LDT 1


// GDT attribute
#define DESC_G_4K 1
#define DESC_D_32 1
#define DESC_L_32bit 0 // flag for 32 bits code
#define DESC_L_64bit 0 // flag for 64 bits code
#define DESC_AVL 0
#define DESC_P 1
#define DESC_DPL_0 0
#define DESC_DPL_1 1
#define DESC_DPL_2 2
#define DESC_DPL_3 3

// for S bit in DESC
#define DESC_S_CODE 1
#define	DESC_S_DATA DESC_S_CODE
#define DESC_S_SYS 0

// type attribute in DESC
#define DESC_TYPE_CODE 8 // x=1 c(consistency)=0 r=0 a(access)=0 
#define DESC_TYPE_DATA 2 // x=0 e(extend upward)=0 w=1 a=0
#define DESC_TYPE_TSS 9 // B=0, not busy

// high 32bits for desc
#define GDT_ATTR_HIGH ((DESC_G_4K<<7) + (DESC_D_32<<6) + (DESC_L_32bit<<5) + (DESC_AVL<<4))
#define GDT_CODE_ATTR_LOW_DPL3 ((DESC_P<<7)+(DESC_DPL_3<<5)+(DESC_S_CODE<<4)+DESC_TYPE_CODE)
#define GDT_DATA_ATTR_LOW_DPL3 ((DESC_P<<7)+(DESC_DPL_3<<5)+(DESC_S_DATA<<4)+DESC_TYPE_DATA)

// attribute for TSS DESC
#define TSS_DESC_D 0
#define TSS_ATTR_HIGH ((DESC_G_4K<<7)+(TSS_DESC_D<<6)+(DESC_L_32bit<<5)+(DESC_AVL<<4)+0x0)
#define TSS_ATTR_LOW ((DESC_P<<7)+(DESC_DPL_0<<5)+(DESC_S_SYS<<4)+DESC_TYPE_TSS)


// selector for kernel
#define SELECTOR_K_CODE ((1<<3)+(TI_GDT<<2)+RPL0)
#define SELECTOR_K_DATA ((2<<3)+(TI_GDT<<2)+RPL0)
#define SELECTOR_K_STACK SELECTOR_K_DATA
#define SELECTOR_K_GS ((3<<3)+(TI_GDT<<2)+RPL0)
// 4th selector is TSS DESC
#define SELECTOR_TSS ((4<<3)+(TI_GDT<<2)+RPL0)
// selector for user
#define SELECTOR_U_CODE ((5<<3)+(TI_GDT<<2)+RPL3)
#define SELECTOR_U_DATA ((6<<3)+(TI_GDT<<2)+RPL3)
#define SELECTOR_U_STACK SELECTOR_U_DATA



// idt-desc attr
#define IDT_DESC_P 1 //present
#define IDT_DESC_DPL0 0
#define IDT_DESC_DPL3 3
#define IDT_DESC_32_TYPE 0xe
#define IDT_DESC_16_TYPE 0x6 

// generate P+DPL+S+TYPE
#define IDT_DESC_ATTR_DPL0 ((IDT_DESC_P<<7)+(IDT_DESC_DPL0<<5)+IDT_DESC_32_TYPE)
#define IDT_DESC_ATTR_DPL3 ((IDT_DESC_P<<7)+(IDT_DESC_DPL3<<5)+IDT_DESC_32_TYPE)

#define PG_SIZE 4096 // size each page


#define NUM_FULL_LINE_CH 80
#define NUM_FULL_SCREEN_LINE 25
#define NUM_FULL_SCREEN_CH NUM_FULL_SCREEN_LINE*NUM_FULL_LINE_CH

#define EFLAGS_MBS (1<<1) // must set this bit
#define EFLAGS_IF_1 (1<<9)
#define EFLAGS_IF_0 (0<<9)
#define EFLAGS_IOPL_3 (3<<12)
#define EFLAGS_IOPL_0 (0<<12)

#define DIV_ROUND_UP(X,STEP) ((X+STEP-1)/(STEP))

#define GDT_BASE 0xc0000903
#define SYS_MEM_SIZE_PTR 0xb03
#define DISK_NUM_PTR 0x475

#define SHELL_PATH  "/bin/shell"
#define BIN_DIR  "/bin"

#define UNUSED __attribute__((unused))

// items in gdt
struct gdt_desc{
	uint16_t limit_low_word;
	uint16_t base_low_word;
	uint8_t base_mid_byte;
	uint8_t attr_low_byte;
	uint8_t limit_high_attr_high;
	uint8_t base_high_byte; 
};


#endif