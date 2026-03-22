#ifndef __INCLUDE_MAGICBOX_SWAP_H
#define __INCLUDE_MAGICBOX_SWAP_H

#include <stdint.h>

struct task_struct;

extern void copy_page_tables(struct task_struct* from,struct task_struct* to,void* page_buf);
extern void swap_page(uint32_t err_code,void* err_vaddr);
extern void write_protect(uint32_t err_code,void* err_vaddr);

#endif