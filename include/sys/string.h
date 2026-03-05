#ifndef __LIB_STRING_H
#define __LIB_STRING_H
#include "stdint.h"
//set $(size) bytes from $(dst_) as $(value) 
extern void memset(void* dst_,uint8_t value,uint32_t size); 
//copy $(size) bytes from dst_ to src_
extern void memcpy(void* dst_,const void* src_,uint32_t size); 
// compare $(size) bytes from the beginning of $(a_) and $(b_) 
// if a_ is bigger than b_, then return +1,else return -1 
extern int8_t memcmp(const void* a_,const void* b_,uint32_t size); 

// return the beginning of the dst_ string
extern char* strcpy(char* dst_,const char* src_);
extern uint32_t strlen(const char* str);
//if a>b, then return 1; if a==b, then return 0; if a<b, return -1
extern int8_t strcmp(const char* a,const char* b); 
// find the first position where ch appears in the str from left to right
extern char* strchr(const char* str,const uint8_t ch); 
// find the first position where ch appears in the str from right to left
extern char* strrchr(const char* str,const uint8_t ch); 
// append the characters of src_ to the end of dst_
extern char* strcat(char* dst_,const char* src_);
// find how many times ch appears in str
extern uint32_t strchrs(const char* str,uint8_t ch);
#endif