#ifndef __LIB_STDIO_H
#define __LIB_STDIO_H
#include "../lib/stdint.h"
#include "../lib/stdbool.h"
typedef char* va_list;
// v is the first arg in variable arguments
#define va_start(ap,v) ap = (va_list)&v;
// t is type
// move to next arg in the stack and return it's value
// because the size of every elements in stack is 4byte in 32bits system
// so ap+=4 to move 4 bytes
#define va_arg(ap,t) *((t*)(ap+=4))
// clear ap
#define va_end(ap) ap=NULL;

extern uint32_t vsprintf(char* str,const char* format,va_list ap);
extern uint32_t printf(const char* format,...);
extern uint32_t sprintf(char* buf,const char* format,...);
extern int itoa(uint32_t value,char* buf_ptr,uint8_t base);
extern bool atoi(char* str,int32_t* buf);
#endif