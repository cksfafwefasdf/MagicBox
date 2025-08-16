#ifndef __LIB_KERNEL_BITMAP_H
#define __LIB_KERNEL_BITMAP_H
#include "../../kernel/global.h"
#include "../stdint.h"
#include "../stdbool.h"
#define BITMAP_MASK 1

struct bitmap{
	uint32_t btmp_bytes_len; // The size of the bitmap
	uint8_t* bits; // The smallest step-length to move.At the same time it is the beginning of the bitmap
};

extern void bitmap_init(struct bitmap* btmp);
// check if bit_idx is 1,if so ,return true,else return false
extern bool bitmap_bit_check(struct bitmap* btmp,uint32_t bit_idx);
// allocate cnt consecutive bits, return 1 if successful, otherwise return -1
extern int bitmap_scan(struct bitmap* btp,uint32_t cnt);
extern void bitmap_set(struct bitmap* btmp,uint32_t bit_idx,int8_t value);
// check the num of the free bit in the bitmap
extern uint32_t bitmap_count(struct bitmap* btmp);
#endif