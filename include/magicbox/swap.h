#ifndef __INCLUDE_MAGICBOX_SWAP_H
#define __INCLUDE_MAGICBOX_SWAP_H

#include <stdint.h>
#include <bitmap.h>
#include <sync.h>
#include <dlist.h>

#define MAX_SWAP_DEVICES 8

struct task_struct;
struct partition;

struct swap_info {
    struct partition* part; // 引用你现有的分区结构
    struct bitmap slot_bitmap; // 该 Swap 分区专属的位图
    uint32_t slot_cnt; // 槽位总数 (sec_cnt / 8)
    uint8_t dev_id; // 给置换算法看的 ID (0-7)
    struct dlist_elem swap_list_tag; // 挂载到全局 swap_list 中
};

extern void copy_page_tables(struct task_struct* from,struct task_struct* to,void* page_buf);
extern void swap_page(uint32_t err_code,void* err_vaddr);
extern void write_protect(uint32_t err_code,void* err_vaddr);
extern void swap_init(void);
extern void do_swapon(struct partition* part);
#endif