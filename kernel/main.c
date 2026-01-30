#include "print.h"
#include "init.h"

int main(void) {
   put_str("enter kernel\n");
   init_all();

   while(1);
   return 0;
}