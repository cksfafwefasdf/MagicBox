/*
    machine mode 
    b => [a~d]l
    w => [a~d]x
    h => [a~d]h
    k => e[a~d]x
*/

#ifndef __LIB_KERNEL_IO_H
#define __LIB_KERNEL_IO_H
#include "stdint.h"

// write 1byte to port
static inline void outb(uint16_t port,uint8_t data){
    // N is a imm-num constrain,means 0~255
    asm volatile ("outb %b1,%w0"::"Nd"(port),"a"(data));
}

// write word_cnt words to port from addr
static inline void outsw(uint16_t port,const void *addr,uint32_t word_cnt){
    // outsw will carry data from ds:esi to port
    // port default in dx
    asm volatile ("cld;rep outsw":"+S"(addr),"+c"(word_cnt):"Nd"(port):"memory");
}

// read 1byte from the port
static inline uint8_t inb(uint16_t port){
    // inb dx,al
    uint8_t data;
    asm volatile ("inb %w1,%b0":"=a"(data):"Nd"(port));
    return data;
}

// read word_cnt words from port,and write them to addr
static inline void insw(uint16_t port,void *addr,uint32_t word_cnt){
    // rep insw ; 
    // es:edi(addr)
    asm volatile ("cld;rep insw":"+c"(word_cnt),"+D"(addr):"Nd"(port):"memory");
}

#endif