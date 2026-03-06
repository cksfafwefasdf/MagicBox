#include <ioctl.h>
#include <device.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <tty.h>
#include <file_table.h>
#include <thread.h>
#include <ide.h>

int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    if (fd >= MAX_FILES_OPEN_PER_PROC) return -EBADF; // 错误码 1：描述符越界

    // 从进程的局部 FD 表中取出全局文件表的下标
    int32_t global_fd = fd_local2global(get_running_task_struct() ,fd);
    if (global_fd == -1) return -EBADF; // 描述符未打开的情况
    
    struct file* file = &file_table[global_fd];
    struct inode* inode = file->fd_inode;

    if (file->fd_inode == NULL) return -EBADF; // 错误码 2：该描述符是空的

    // 和 sys_open 一样，进行分发
    switch (inode->i_type) {
        case FT_CHAR_SPECIAL:
            // 字符设备分发
            if (MAJOR(inode->i_rdev) == TTY_MAJOR) {
                return tty_ioctl(file, cmd, arg);
            }
            return -ENOTTY;

        case FT_BLOCK_SPECIAL:
            // 块设备分发
            if (MAJOR(inode->i_rdev) == IDE_MAJOR) {
                return ide_ioctl(file, cmd, arg);
            }
            return -ENOTTY;

        default:
            // 普通文件或目录目前可能不支持 ioctl
            return -ENOTTY;
    }
}
