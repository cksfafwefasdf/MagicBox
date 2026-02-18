#include "print.h"
#include "init.h"

int main(void) {
   put_str("enter kernel\n");
   early_init();

   while(1);
   return 0;
}