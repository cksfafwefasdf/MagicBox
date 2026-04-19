#include <string.h>
#include <stdint.h>
#include <assert.h>

// 使用 rep （类似于循环展开，一次展开四字节，而不再是逐字节）
void memset(void* dst, uint8_t value, uint32_t size) {
    assert(dst != NULL);

    uint32_t value32 = (uint32_t)value | ((uint32_t)value << 8) | 
                       ((uint32_t)value << 16) | ((uint32_t)value << 24);
    uint32_t dwords = size / 4;
    uint32_t bytes = size % 4;

    asm volatile (
        "rep; stosl"  // 填充 4 字节块
        : "+D"(dst), "+c"(dwords)
        : "a"(value32)
        : "memory"
    );

    asm volatile (
        "rep; stosb"  // 填充剩下的字节
        : "+D"(dst), "+c"(bytes)
        : "a"((uint32_t)value)
        : "memory"
    );
}

void memcpy(void* dst, const void* src, uint32_t size) {
    assert((dst != NULL) && (src != NULL));
    
    // 分成两部分，先拷贝 4 字节块，剩下的逐字节拷贝
    uint32_t dwords = size / 4;
    uint32_t bytes = size % 4;

    // 利用内联汇编调用 rep movsd 来进行 4 字节的拷贝
    asm volatile (
        "rep; movsl"
        : "+D"(dst), "+S"(src), "+c"(dwords)
        : : "memory"
    );

    // 拷贝剩下的不足 4 字节的部分
    asm volatile (
        "rep; movsb"
        : "+D"(dst), "+S"(src), "+c"(bytes)
        : : "memory"
    );
}

int8_t memcmp(const void* a_,const void* b_,uint32_t size){
	const uint8_t* a = a_;
	const uint8_t* b = b_;
	assert(a!= NULL||b!=NULL);
	while (size-->0){
		if(*a!=*b) return *a>*b?1:-1;
		a++;b++;
	}
	return 0;
} 

char* strcpy(char* dst_,const char* src_){
	assert(dst_!=NULL&&src_!=NULL);
	char *r = dst_;
	while ((*dst_++=*src_++));
	return r;
}

uint32_t strlen(const char* str){
	assert(str!=NULL);
	const char* p = str;
	while(*p++); // end of str is \0 (value is 0)
	return (p-str-1);
}

int8_t strcmp(const char* a,const char* b){
	assert(a!=NULL && b!=NULL);
	while(*a!=0&&*a==*b){
		a++;
		b++;
	}
	return *a<*b?-1:*a>*b;
} 

char* strchr(const char* str,const uint8_t ch){
	assert(str!=NULL);
	while(*str!='\0'){
		if(*str==ch) return (char*) str;
		str++;
	}
	return NULL;
} 


char* strrchr(const char* str,const uint8_t ch){
	assert(str!=NULL);
	const char* last_char = NULL;
	while(*str!='\0'){
		if(*str==ch) last_char = str;
		str++;
	}
	return (char *)last_char;
}

char* strcat(char* dst_,const char* src_){
	assert(dst_!=NULL&&src_!=NULL);
	char* str = dst_;
	while(*str++);
	--str; // go back to '\0' 
	while ((*str++ = *src_++));
	return dst_;
}

uint32_t strchrs(const char* str,uint8_t ch){
	assert(str!=NULL);
	uint32_t cnt = 0;
	const char *p = str;
	while(*p!='\0'){
		if(*p==ch) cnt++;
		p++;
	}	
	return cnt;
}

char* strncat(char* dst_, const char* src_, uint32_t n) {
    assert(dst_ != NULL && src_ != NULL);
    char* str = dst_;
    while (*str++);
    --str; // 移到 \0 的位置
    
    // 拷贝 n 个字符
    uint32_t i = 0;
    while (i < n && *src_ != '\0') {
        *str++ = *src_++;
        i++;
    }
    *str = '\0'; // 强制封口
    return dst_;
}