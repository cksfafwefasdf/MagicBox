#include "char_dev.h"
#include "tty.h"

// 字符设备跳转表
// 类似于 Linux 的 crw_table (Character Read/Write Table)
struct char_device crw_table[MAX_CHAR_DEVS];

// 注册函数，驱动初始化时调用
void register_char_dev(uint8_t major, struct file_operations* fops, char* name) {
    if (major >= MAX_CHAR_DEVS) return;
    crw_table[major].fops = fops;
    crw_table[major].name = name;
}

// 找设备的辅助函数
struct file_operations* get_char_dev_fops(uint8_t major) {
    if (major >= MAX_CHAR_DEVS) return NULL;
    return crw_table[major].fops;
}