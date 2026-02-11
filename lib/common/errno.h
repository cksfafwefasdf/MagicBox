#ifndef __LIB_COMMON_ERRNO_H
#define __LIB_COMMON_ERRNO_H
#define EPERM 1 // Operation not permitted
#define ENOENT 2 // No such file or directory
#define ESRCH 3  // No such process
#define EINTR 4 // Interrupted system call
#define EIO 5  // I/O error
#define ENXIO 6 // No such device or address
#define E2BIG 7 // Argument list too long 
#define ENOEXEC 8 // Exec format error 
#define EBADF 9 // Bad file number
#define EAGAIN 11 // Try again 
#define EFAULT 14 // Bad address.
#define EINVAL 22 // Invalid argument 
#define ENOTTY 25 // Not a typewriter (这不是打印机/终端)
#define EPIPE 32 //Broken pipe 
#endif