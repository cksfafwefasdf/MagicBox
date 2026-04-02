#include <ioctl.h>
#include <device.h>
#include <unitype.h>
#include <errno.h>
#include <stdint.h>
#include <tty.h>
#include <file_table.h>
#include <thread.h>
#include <stdio-kernel.h>
#include <ide.h>

int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    if (fd >= MAX_FILES_OPEN_PER_PROC) return -EBADF; // 错误码 1：描述符越界

    // 从进程的局部 FD 表中取出全局文件表的下标
    int32_t global_fd = fd_local2global(get_running_task_struct() ,fd);
    if (global_fd == -1) return -EBADF; // 描述符未打开的情况
    
    struct file* file = &file_table[global_fd];
    struct inode* inode = file->fd_inode;

    if (file->fd_inode == NULL) return -EBADF; // 错误码 2：该描述符是空的

    if(file->f_op!=NULL&&file->f_op->ioctl!=NULL){
        return file->f_op->ioctl(inode,file, cmd, arg);
    }else{
        printk("sys_ioctl: this type of device do not support ioctl!\n");
        return -ENOTTY;
    }
}
