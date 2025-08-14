#ifndef __LIB_ASSERT_H
#define __LIB_ASSERT_H
extern void panic_spin_user(char* filename,int line,const char* func,const char* condition);

#define panic(...) panic_spin_user(__FILE__,__LINE__,__func__,__VA_ARGS__)

#ifdef NDEBUG
    #define assert(CONDITION) ((void)0)
#else 
    #define assert(CONDITION) if(!(CONDITION)){panic(#CONDITION);}
#endif // NDEBUG

#endif