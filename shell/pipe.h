#ifndef __SHELL_PIPE_H
#define __SHELL_PIPE_H
#define PIPE_FLAG 0xFFFF

extern bool is_pipe(uint32_t local_fd);
extern int sys_pipe(int32_t pipefd[2]);
extern uint32_t pipe_read(int32_t fd,void* buf,uint32_t count);
extern uint32_t pipe_write(int32_t fd,const void* buf,uint32_t count);
extern void sys_fd_redirect(uint32_t old_local_fd, uint32_t new_local_fd);


#endif