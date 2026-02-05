#include "stdio.h"
#include "console.h"
#include "stdio-kernel.h"
#include "unistd.h"

void printk(const char* format,...){
	va_list args;
	va_start(args,format);
	char buf[PRINT_BUF_SIZE] = {0};
	vsprintf(buf,format,args);
	va_end(args);
	console_put_str(buf);
}