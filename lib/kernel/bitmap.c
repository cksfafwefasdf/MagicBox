#include "bitmap.h"
#include "stdint.h"
#include "string.h"
#include "debug.h"
#include "interrupt.h"
#include "print.h"

void bitmap_init(struct bitmap* btmp){
	memset(btmp->bits,0,btmp->btmp_bytes_len);
}

// check if bit_idx is 1,if so, return true(1),otherwise return false(0)
bool bitmap_bit_check(struct bitmap* btmp,uint32_t bit_idx){
	uint32_t byte_idx = bit_idx/8;
	uint32_t offset_in_byte = bit_idx%8;
	return (btmp->bits[byte_idx]&(BITMAP_MASK<<offset_in_byte)); 
}
// allocate cnt consecutive bits,if successful, return 1,otherwise return -1 
int bitmap_scan(struct bitmap* btmp,uint32_t cnt){
	uint32_t idx_byte = 0;
	while((0xff==btmp->bits[idx_byte])&&(idx_byte<btmp->btmp_bytes_len)){
		idx_byte++;
	}
	ASSERT(idx_byte<btmp->btmp_bytes_len);
	if(idx_byte==btmp->btmp_bytes_len){
		return -1;
	}

	int idx_bit=0;
	while ((uint8_t)(BITMAP_MASK<<idx_bit)&(btmp->bits[idx_byte])){
		idx_bit++;
	}

	int bit_idx_start = idx_byte*8 + idx_bit;
	if(cnt==1) return bit_idx_start;

	uint32_t bit_remain = (btmp->btmp_bytes_len*8 - bit_idx_start);

	uint32_t next_bit = bit_idx_start+1;
	uint32_t count = 1;

	bit_idx_start = -1;
	while(bit_remain-->0){
		if(!(bitmap_bit_check(btmp,next_bit))){
			count++;
		}else{
			count=0;
		}
		if(count==cnt){
			bit_idx_start = next_bit - cnt +1;
			break;
		}
		next_bit++;
	}
	return bit_idx_start;
	
}

void bitmap_set(struct bitmap* btmp,uint32_t bit_idx,int8_t value){
	ASSERT((value==0)||(value==1));
	uint32_t byte_idx = bit_idx/8;
	uint32_t offset_in_byte = bit_idx%8;

	if(value){
		btmp->bits[byte_idx] |= (BITMAP_MASK<<offset_in_byte);
	}else{
		btmp->bits[byte_idx] &= ~(BITMAP_MASK<<offset_in_byte);
	}
}