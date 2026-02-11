#include "assert.h"
#include "stdio.h"

void panic_spin_user(char* filename,int line,const char* func,const char* condition){
    printf("\n\n\n!!!!!!!!!! error !!!!!!!!!!\n");
    printf("filename: %s\nline: 0x%x\nfunction: %s\ncondition: %s\n",filename,line,(char*)func,(char*)condition);
    printf("!!!!!!!!!! error !!!!!!!!!!\n");
    while (1);
}