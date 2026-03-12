#ifndef __INCLUDE_MAGICBOX_PIPE_H
#define __INCLUDE_MAGICBOX_PIPE_H
#include <ioqueue.h>
#include <stdint.h>

struct file;
struct inode;

// 管道不是不支持随机访问的，因此它不支持 lseek
// pipefd[0] 是读端，pipefd[1] 是写端
#define PIPE_READ 0
#define PIPE_WRITE 1

struct pipe_inode_info {
    struct pipe* base; // 指向缓冲区 
    uint32_t reader_count; // 读端打开次数
    uint32_t writer_count; // 写端打开次数
};

struct pipe {
    struct ioqueue queue; // 用于进行数据交换的区域
};

// pipe_release pipe_write pipe_read 可以融入到 sys_close sys_write sys_read 中
// 但是 sys_pipe 没办法弄到 sys_open 中！因为 sys_open 是基于路径的，而匿名管道没有路径
// 而另外三个函数是可以融入的，因为这三个函数是基于句柄的，pipe 有句柄
// pipe_release pipe_write pipe_read fifo也要用，不能弄成 static
extern int32_t pipe_release(struct inode* inode, struct file* file);
extern int32_t pipe_write(struct inode* inode, struct file* file, char* buf, int32_t count);
extern int32_t pipe_read(struct inode* inode, struct file* file, char* buf, int32_t count);

extern int32_t sys_pipe(int32_t pipefd[2]);
extern int32_t init_pipe(struct inode* inode);

extern struct file_operations pipe_file_operations;

#endif