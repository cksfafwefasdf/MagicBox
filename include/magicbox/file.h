#ifndef __INCLUDE_MAGICBOX_FILE_H
#define __INCLUDE_MAGICBOX_FILE_H

#include <stdint.h>

struct partition;
struct file;

extern int32_t file_close(struct file* file);
extern int32_t file_open(struct partition* part, uint32_t inode_no,uint8_t flag);
extern int32_t file_mmap(struct file* file, uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, uint32_t offset);


#endif
