#include "stdio.h"
#include "global.h"
#include "stdint.h"
#include "string.h"
#include "syscall.h"
#include "unistd.h"

int itoa(uint32_t value,char* buf_ptr,uint8_t base){
	if(value==0){
		buf_ptr[0] = '0';
		buf_ptr[1] = '\0';
		return 1;
	}
	uint32_t remain = value%base;
	uint32_t i = value/base;
	int len = 0;
	while(i!=0){
		if(remain<10){
			*(buf_ptr+len) = remain+'0';
		}else if(remain>=10&&remain<=15){
			remain-=10;
			*(buf_ptr+len) = remain+'A';
		}
		len++;
		remain = i%base;
		i/=base;
	}
	if(remain!=0){
		
		*(buf_ptr+len)= (remain<10?remain+'0':remain-10+'A');
		len++;
	}
	*(buf_ptr+len) = '\0';
	// reverse str
	int _i=0,_j=len-1;
	for(;_i<_j;_i++,_j--){
		char tmp = buf_ptr[_i];
		buf_ptr[_i] = buf_ptr[_j];
		buf_ptr[_j] = tmp;
	}
	return len;
}

bool atoi(char* str,int32_t* buf){
	*buf = 0;
	int32_t idx=0;
	bool is_negative = false;
	while(!((str[idx]>='0'&&str[idx]<='9')||str[idx]=='-')){
		idx++;
	}
	if(str[idx]=='-'){
		is_negative = true;
		idx++;
	}
	

	while(str[idx]!=0&&str[idx]>='0'&&str[idx]<='9'){
		(*buf)*=10;
		(*buf)+=str[idx]-'0';
		idx++;
	}
	
	if(is_negative){
		*buf=0-*buf;
	}

	if(str[idx]!=0){
		printf("atoi: illegal character %c!\n",str[idx]);
		return false;
	}
	return true;
}

// format the parameter [ap] according to [format], and output it to [str]
// return the length of the modified [str]
uint32_t vsprintf(char* str,const char* format,va_list ap){
	char* buf_ptr = str;
	const char* index_ptr = format;
	char index_char = *index_ptr;
	int32_t arg_int;
	// while index_char != '\0'
	while(index_char){
		if(index_char!='%'){
			// copy the character
			*(buf_ptr++) = index_char;
			// format ptr move to next
			index_char = *(++index_ptr);
			continue;
		}
		// skip %
		index_char = *(++index_ptr);
		int len = 0;
		char* arg_str;
		switch(index_char){
			case 'x':
				// [ap] points to the first arg in variable args [format] at first
				// so we need move [ap] to the next elem and get the arg1 firstly
				arg_int = va_arg(ap,int);
				//itoa_recur(arg_int,&buf_ptr,16);

				len = itoa(arg_int,buf_ptr,16);
				buf_ptr+=len;
				
				index_char = *(++index_ptr);
				// skip format character and update index_char
				break;
			case 'c':
				// copy the arg to the buf directly
				*(buf_ptr++) = va_arg(ap,char);
				index_char = *(++index_ptr);
				break;
			case 'd':
				arg_int = va_arg(ap,int);
				// if it is negtive number
				if(arg_int<0){
					// convert it positive and add a minus - before
					arg_int = 0-arg_int;
					*buf_ptr++ = '-';
				}
				len = itoa(arg_int,buf_ptr,10);
				buf_ptr+=len;

				index_char = *(++index_ptr);
				break;
			case 's':
				arg_str = va_arg(ap,char*);
				strcpy(buf_ptr,arg_str);
				buf_ptr+=strlen(arg_str);
				index_char = *(++index_ptr);
				break;
		}
	}

	return strlen(str);
}

// format and output [format] to [buf]
uint32_t sprintf(char* buf,const char* format,...){
	va_list args;
	uint32_t retval;
	va_start(args,format);
	retval = vsprintf(buf,format,args);
	va_end(args);
	return retval;
}

// format and output [format] to stdout
uint32_t printf(const char* format,...){
	va_list args;
	va_start(args,format);
	char buf[PRINT_BUF_SIZE] = {0};
	vsprintf(buf,format,args);
	va_end(args);
	// because printf is called by user
	// so we need call syscall write
	return write((int)stdout_no,buf,strlen(buf));
}