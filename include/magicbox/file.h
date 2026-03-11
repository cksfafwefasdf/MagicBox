#ifndef __INCLUDE_MAGICBOX_FILE_H
#define __INCLUDE_MAGICBOX_FILE_H

#include <stdint.h>

struct partition;
struct file;

extern int32_t file_close(struct file* file);
extern int32_t file_open(struct partition* part, uint32_t inode_no,uint8_t flag);


#endif