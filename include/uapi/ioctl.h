#ifndef __INCLUDE_UAPI_IOCTL_H
#define __INCLUDE_UAPI_IOCTL_H

#include <stdint.h>

// 模仿 Linux 的 ioctl 编码格式，定义一个命令的各个字段长度
#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2

// 位移定义
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

// 方向定义
#define _IOC_NONE      0U
#define _IOC_WRITE     1U
#define _IOC_READ      2U

// 构造命令的宏
#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

/* 方便使用的辅助宏 */
#define _IO(type, nr)          _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size)   _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)   _IOC(_IOC_WRITE, (type), (nr), sizeof(size))


#define BLK_MAGIC 0x12
#define BLKGETSIZE   _IO(BLK_MAGIC, 0x45) // 对应 0x1245
#define BLKGETSIZE64 _IOR(BLK_MAGIC, 0x72, uint64_t) // 对应 0x80081272

// TTY 子系统的幻数定义为 'T'
#define TTY_MAGIC 'T' // 即 0x54

// i386 通常不使用上面那种复杂的定义方式，我们直接硬编码吧
// 磁盘操作的暂时不修改了，之后出问题了再改
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404

#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

#define TIOCGWINSZ  0x5413 // 获取窗口大小，ash 有时也会查这个

extern int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg);

#endif