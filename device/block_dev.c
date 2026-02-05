#include "block_dev.h"

struct block_device brw_table[MAX_BLOCK_DEVS];

// 找设备的辅助函数
struct file_operations* get_block_dev_fops(uint8_t major) {
    if (major >= MAX_BLOCK_DEVS) return NULL;
    return brw_table[major].fops;
}

// 注册块设备驱动（比如 IDE 驱动初始化时调用）
void register_block_dev(uint8_t major, struct file_operations* fops, char* name) {
    if (major >= MAX_BLOCK_DEVS) return;
    brw_table[major].fops = fops;
    brw_table[major].name = name;
}