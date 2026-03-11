#ifndef __INCLUDE_UAPI_SIFS_FILE_H
#define __INCLUDE_UAPI_SIFS_FILE_H

#include <stdint.h>
#include <unistd.h>
#include <fs_types.h>

struct partition;

extern int32_t sifs_file_write(struct file* file,const void* buf,uint32_t count);
extern int32_t sifs_file_read(struct file* file,void* buf,uint32_t count);

// extern uint32_t prog_size; // the size of the program being executed
#endif