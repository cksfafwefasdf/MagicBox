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
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) return -EBADF; // 描述符越界

    // 从进程的局部 FD 表中取出全局文件表的下标
    int32_t global_fd = fd_local2global(get_running_task_struct() ,fd);
    if (global_fd == -1) return -EBADF; // 描述符未打开的情况
    
    struct file* file = &file_table[global_fd];
    struct inode* inode = file->fd_inode;

    if (file->fd_inode == NULL) return -EBADF; // 该描述符是空的

    if(file->f_op!=NULL&&file->f_op->ioctl!=NULL){
        return file->f_op->ioctl(inode,file, cmd, arg);
    }
    
    // 如果是普通文件或目录，它们通常不支持 ioctl
    // 这里静默返回 ENOTTY，避免 printk 刷屏
    if (file->fd_inode->i_type != FT_REGULAR && 
        file->fd_inode->i_type != FT_DIRECTORY &&
        file->fd_inode->i_type != FT_PIPE) {
        // 只有在遇到“本该支持却没支持”的设备时才打印错误，比如 TTY
        printk("sys_ioctl: type 0x%x does not support ioctl\n", file->fd_inode->i_type);
    }

    return -ENOTTY;
}
