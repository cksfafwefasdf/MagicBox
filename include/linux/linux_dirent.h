#ifndef __INCLUDE_LINUX_LINUX_DIRENT_H
#define __INCLUDE_LINUX_LINUX_DIRENT_H

#include <stdint.h>

struct linux_dirent64 {
    uint64_t        d_ino;
    int64_t         d_off;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[];
};

#endif