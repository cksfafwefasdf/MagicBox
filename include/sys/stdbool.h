#ifndef __LIB_STDBOOL_H
#define __LIB_STDBOOL_H
#include "stdint.h"
#define bool int8_t
#define true 1
#define false 0
#define __bool_true_false_are_defined 1 //To indicate whether bool is defined 
#endif