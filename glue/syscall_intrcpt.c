#include <syscall_intrcpt.h>
#include <thread.h>
#include <stdint.h>
#include <stdio-kernel.h>
#include <errno.h>
#include <unistd_32.h>
#include <fs.h>
#include <ioctl.h>
#include <memory.h>
#include <syscall-init.h>
#include <wait_exit.h>
#include <linux_stat.h>
#include <fs_types.h>
#include <thread.h>
#include <file_table.h>
#include <debug.h>
#include <linux_dirent.h>
#include <time.h>
#include <exec.h>
#include <fork.h>
#include <unitype.h>
#include <fcntl.h>
#include <poll.h>

// 基于 i386 Linux 0x80 中断的参数约定
// 强制转换成 uint32_t 是为了防止在进行地址运算或逻辑判断时产生符号位扩展的意外。
#define ARG1(stack) ((uint32_t)(stack)->ebx)
#define ARG2(stack) ((uint32_t)(stack)->ecx)
#define ARG3(stack) ((uint32_t)(stack)->edx)
#define ARG4(stack) ((uint32_t)(stack)->esi)
#define ARG5(stack) ((uint32_t)(stack)->edi)
#define ARG6(stack) ((uint32_t)(stack)->ebp)

typedef int32_t (*musl_syscall_handler)(struct intr_stack*);

musl_syscall_handler musl_syscall_table[NR_syscalls];

// 用于和 Linux 的 writev 调用对齐
// 我们用多次 write 操作来简单的模拟 writev 操作
// 因此我们的 writev 操作并不具备 Linux 那样的原子语义
struct linux_iovec {
    void* iov_base;
    uint32_t iov_len;
};

struct linux_mmap2_args {
    uint32_t addr;
    uint32_t len;
    uint32_t prot;
    uint32_t flags;
    int32_t fd;
    uint32_t pgoff;
};

static int32_t musl_sys_writev(int32_t fd, const struct linux_iovec* iov, int32_t iovcnt) {
    if (iov == NULL || iovcnt < 0) {
        return -EINVAL;
    }

    int32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        uint32_t iov_len = iov[i].iov_len;
        if (iov_len == 0) {
            continue;
        }

        int32_t written = sys_write(fd, iov[i].iov_base, iov_len);
        if (written < 0) {
            if (total > 0) {
                return total;
            }
            return written;
        }

        total += written;
        if ((uint32_t)written < iov_len) {
            return total;
        }
    }

    return total;
}

static uint32_t musl_sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff) {
    if (pgoff > (0xffffffffU >> 12)) {
        return (uint32_t)MAP_FAILED;
    }
    return sys_mmap_direct(addr, len, prot, flags, fd, pgoff << 12);
}

static int32_t linux_sys_stat64(const char* pathname, struct linux_stat64* buf,bool follow_link) {
    struct stat kstat; // MagicBox 内核定义的 stat
    memset(buf, 0, sizeof(struct stat));
    // 调用 MagicBox 原生的 sys_stat 获取数据
    int32_t ret = -ENOENT;
    if (follow_link) {
        ret = sys_stat(pathname, &kstat);
    } else {
        ret = sys_lstat(pathname, &kstat);
    }
    
    if (ret < 0) return ret;

    // 格式转换，从 stat (MagicBox) 搬运到 linux_stat64 (Linux ABI)
    memset(buf, 0, sizeof(struct linux_stat64));
    buf->st_dev = kstat.st_dev;
    buf->st_ino = kstat.st_ino; // 低位 Inode
    buf->__st_ino = kstat.st_ino;
    buf->st_mode = kstat.st_mode;
    buf->st_nlink = kstat.st_nlink;
    buf->st_uid = kstat.st_uid;
    buf->st_gid = kstat.st_gid;
    buf->st_rdev = kstat.st_rdev;
    buf->st_size = kstat.st_size; // 这里是 int64，已经对齐了 stat64 的要求
    
    buf->st_blksize = kstat.st_blksize; 
    buf->st_blocks = (kstat.st_size + 511) / SECTOR_SIZE; // 以 512 字节块为单位计

    // 时间戳转换
    buf->st_atime = kstat.st_atime;
    buf->st_atime_nsec = 0; // 我们暂不支持纳秒，填 0 即可
    buf->st_mtime = kstat.st_mtime;
    buf->st_mtime_nsec = 0;
    buf->st_ctime = kstat.st_ctime;
    buf->st_ctime_nsec = 0;

    buf->st_ino = kstat.st_ino; // stat64 末尾还有一个真正的 64 位 inode 字段

    return 0;
}

// linux_sys_fstat64 对接 0xc5
// 意为使用 fd 来获取文件信息而不是path
static int32_t linux_sys_fstat64(int32_t fd, struct linux_stat64* buf) {
    // 检查 fd 是否越界
    if (fd < 0 || fd >= MAX_FILES_OPEN_PER_PROC) {
        return -EBADF;
    }

    // 从当前进程的 fd_table 中获取文件结构
    struct task_struct* cur = get_running_task_struct();
    int32_t global_fd = fd_local2global(cur,fd);
    struct file* file = &file_table[global_fd];

    // 检查该 fd 是否指向一个有效的 inode
    if (file->fd_inode == NULL) {
        return -EBADF;
    }

    struct inode* inv = file->fd_inode;

    // 填充 linux_stat64 结构体
    // 先清空，防止内核栈数据泄露
    memset(buf, 0, sizeof(struct linux_stat64));

    buf->st_dev = inv->i_dev;
    buf->__st_ino = inv->i_no;     // 传统 32 位 inode
    buf->st_mode = inv->i_mode;
    buf->st_nlink = 1;              // 暂时硬编码为 1
    buf->st_uid = 0;                // root
    buf->st_gid = 0;                // root
    buf->st_rdev = inv->i_dev;      // 简单起见同 st_dev
    buf->st_size = inv->i_size;     // 64 位文件大小
    
    
    buf->st_blksize = inv->i_sb->s_block_size; 
    buf->st_blocks = (inv->i_size + 511) / SECTOR_SIZE;

    buf->st_atime = inv->i_atime;
    buf->st_atime_nsec = 0;
    buf->st_mtime = inv->i_mtime;
    buf->st_mtime_nsec = 0;
    buf->st_ctime = inv->i_ctime;
    buf->st_ctime_nsec = 0;

    // 结构体末尾的真实 64 位 Inode 字段
    buf->st_ino = (unsigned long long)inv->i_no;

    return 0; // 成功返回 0
}

// 用户态的 readdir 函数底层会进行这个调用
static int32_t linux_sys_getdents64(int32_t fd, void* dirp, uint32_t count) {
    struct task_struct* cur = get_running_task_struct();
    int32_t _fd = fd_local2global(cur, fd);
    struct file* f = &file_table[_fd];
    
    if (f->fd_inode == NULL || f->fd_inode->i_type != FT_DIRECTORY) return -EBADF;

    uint32_t bytes_written = 0;
    struct dirent kde;

    while (bytes_written < count) {
        // 记录读取前的偏移量
        uint32_t last_pos = f->fd_pos;

        // status == 0 表示成功读到一个
        int status = f->f_op->readdir(f->fd_inode, f, &kde, 0);
        if (status != 0) break; // 读取结束或失败

        // 计算 Linux 格式需要的长度 (对齐到 8 字节)
        int name_len = strlen(kde.d_name);
        // 19是linux_dirent64头部固定长度
        // 由于这个结构体里面有一个柔性数组，所以最好不要用sizeof来计算头部的大小
        // 不然编译器编译后会给我们带来很多问题！
        int linux_reclen = (19 + name_len + 1 + 7) & ~7; 

        // 检查缓冲区是否够用
        if (bytes_written + linux_reclen > count) {
            // 放不下了，必须把文件指针退回到读取这一条之前
            // 否则这一条数据在下一次系统调用时就被跳过了
            f->fd_pos = last_pos; 
            break; 
        }

        // 手动填充用户缓冲区
        struct linux_dirent64* udir = (struct linux_dirent64*)((char*)dirp + bytes_written);
        udir->d_ino = (uint64_t) kde.d_ino;
        // Linux 规范中，d_off 应该指向下一个条目的起始偏移
        udir->d_off = (int64_t) f->fd_pos; // 记录当前读取到的偏移
        udir->d_reclen = (unsigned short)linux_reclen;
        udir->d_type = kde.d_type;
        // 我们的 FT_REGULAR 等与 Linux 一致，不用映射 
        // 拷贝文件名并确保末尾填充 0（防止泄露内核栈余温）
        strcpy(udir->d_name, kde.d_name);

        // 清理对齐产生的填充位
        int actual_data_len = 19 + name_len + 1;
        if (linux_reclen > actual_data_len) {
            memset((char*)udir + actual_data_len, 0, linux_reclen - actual_data_len);
        }

        bytes_written += linux_reclen;
    }

    return bytes_written; // 返回实际填入缓冲区的字节数
}

static int32_t do_default(struct intr_stack* stack){
    printk("Unknown Syscall 0x%x\n", stack->eax);
    // 按照 Linux 规范，未实现的调用通常返回 -38 (ENOSYS)
    return -ENOSYS; 
}

static int32_t do_getpid(struct intr_stack* stack UNUSED){
    return sys_getpid();
}

// exit_group 和 exit 都使用这个函数
// exit_group 会杀死一个进程的所有线程
// 由于我们现在的用户进程都只对应一个内核线程，因此直接用exit就行
static int32_t do_exit_default(struct intr_stack* stack){
    // exit(status) 的参数在 ebx 里 
#ifdef DEBUG_SYSCALL_INTRCPT
    printk("Process %d exiting with status %d\n", get_running_task_struct()->pid, ARG1(stack)); 
#endif

    sys_exit(ARG1(stack));

    // 理论上不应该到这
    PANIC("do_exit_default: should not be here!");
    return 0;
}

static int32_t do_writev(struct intr_stack* stack){
    return musl_sys_writev((int32_t)ARG1(stack),
                            (const struct linux_iovec*)ARG2(stack),
                            (int32_t)ARG3(stack));
}

static int32_t do_ioctl(struct intr_stack* stack){
    // uint32_t cmd = ARG2(stack);
    // printk("do_ioctl: ioctl fd=%d, cmd=0x%x\n", ARG1(stack), cmd);
    return sys_ioctl((int32_t)ARG1(stack),
                    (uint32_t)ARG2(stack),
                    (uint32_t)ARG3(stack));
}

static int32_t do_brk(struct intr_stack* stack){
    return (int32_t)sys_brk((uint32_t)ARG1(stack));
}

static int32_t do_mmap2(struct intr_stack* stack){
    return (int32_t)musl_sys_mmap2((uint32_t)ARG1(stack),
                                    (uint32_t)ARG2(stack),
                                    (uint32_t)ARG3(stack),
                                    (uint32_t)ARG4(stack),
                                    (int32_t)ARG5(stack),
                                    (uint32_t)ARG6(stack));
}

static int32_t do_munmap(struct intr_stack* stack){
    return sys_munmap((uint32_t)ARG1(stack),
                        (uint32_t)ARG2(stack));  
}   

static int32_t do_madvise(struct intr_stack* stack UNUSED){
    // 这是一个malloc的性能优化函数，不是必须的，为了防止报unknown警告，我直接no-op返回
    return 0;
}

static int32_t do_open(struct intr_stack* stack){
    const char* path = (const char*)ARG1(stack);
    uint32_t linux_flags = ARG2(stack);
    uint8_t kernel_flags = 0;

    // 转换读写模式 (Linux 的 RDONLY 是 0，必须特殊处理)
    uint32_t mode = linux_flags & 3; // 取低 2 位
    if (mode == 0) kernel_flags |= O_RDONLY;      // Linux 0 -> 1
    else if (mode == 1) kernel_flags |= O_WRONLY; // Linux 1 -> 2
    else if (mode == 2) kernel_flags |= O_RDWR;   // Linux 2 -> 4

    //  转换状态标志
    if (linux_flags & 0x40)  kernel_flags |= O_CREATE; // Linux 0x40 -> 8
    if (linux_flags & 0x80) kernel_flags |= O_EXCL; // Linux 0x80 ->  64
    if (linux_flags & 0x200) kernel_flags |= O_TRUNC;  // Linux 0x200 -> 16
    if (linux_flags & 0x400) kernel_flags |= O_APPEND; // Linux 0x400 -> 32
    if (linux_flags & 0x800)  kernel_flags |= O_NONBLOCK; // 

    return sys_open(path, kernel_flags);
}

static int32_t do_write(struct intr_stack* stack){
    // EBX: fd, ECX: buf, EDX: count
    return sys_write((int32_t)ARG1(stack), 
                    (void*)ARG2(stack), 
                    (uint32_t)ARG3(stack));
}

static int32_t do_llseek(struct intr_stack* stack){
#ifdef DEBUG_SYSCALL_INTRCPT
    printk("Intrcpt: warning, use lseek as llseek\n");
#endif
            int32_t fd = (int32_t)ARG1(stack);
            int32_t offset = (int32_t)ARG3(stack); // 取低32位
            int64_t* res_ptr = (int64_t*)ARG4(stack);

    // 对齐 whence: Linux(0,1,2) -> 我们的(1,2,3)
    uint32_t linux_whence = ARG5(stack);
    uint32_t kernel_whence = linux_whence + 1;

    // 边界检查，防止用户传入非法值导致 ASSERT 触发
    if (kernel_whence < 1 || kernel_whence > 3) {
        return -EINVAL;
    }

    int32_t new_pos = sys_lseek(fd, offset, kernel_whence);

    if (new_pos < 0) {
        return new_pos; // 此时 ret 为 -1 或错误码
    } else {
        if (res_ptr != NULL) {
            *res_ptr = (int64_t)new_pos;
        }
        return 0; // 成功返回 0
    }
}

static int32_t do_read(struct intr_stack* stack){
    // EBX: fd, ECX: buf, EDX: count
    int32_t fd = (int32_t)ARG1(stack);
    void* buf = (void*)ARG2(stack);
    uint32_t count = (uint32_t)ARG3(stack);

    return sys_read(fd, buf, count);
}

static int32_t do_close(struct intr_stack* stack){
    // EBX: fd
    int32_t fd = (int32_t)ARG1(stack);

    return sys_close(fd);
}

static int32_t do_stat64(struct intr_stack* stack){
     // 通过路径来获取文件信息
    const char* path = (const char*)ARG1(stack);
    struct linux_stat64* linux_buf = (struct linux_stat64*)ARG2(stack);
    
    return linux_sys_stat64(path, linux_buf,true);
}

static int32_t do_fstat64(struct intr_stack* stack){
    int32_t fd = (int32_t)ARG1(stack);
    struct linux_stat64* linux_buf = (struct linux_stat64*)ARG2(stack);
    
    return linux_sys_fstat64(fd, linux_buf);
}

static int32_t do_getdents64(struct intr_stack* stack){
    // 用户态的 readdir 函数底层会进行这个调用
    return linux_sys_getdents64((int32_t)ARG1(stack), (void*)ARG2(stack), (uint32_t)ARG3(stack));
}

static int32_t do_unlink(struct intr_stack* stack){
    const char* path = (const char*)ARG1(stack);
    return sys_unlink(path);
}

static int32_t do_time(struct intr_stack* stack){
    uint32_t* tloc = (uint32_t*)ARG1(stack);
    int32_t now = sys_time(); // 返回 Unix 秒数
    if (tloc) *tloc = now;
    return now;
}

static int32_t do_gettimeofday(struct intr_stack* stack) {
    struct linux_timeval {
        uint32_t tv_sec;
        uint32_t tv_usec;
    } *tv = (struct linux_timeval*)ARG1(stack);

    if (tv) {
        tv->tv_sec = sys_time(); // 获取内核当前的 Unix 时间戳
        tv->tv_usec = 0; // 暂时不给微秒级精度没关系
    }
    return 0;
}

// 现代 C 库（例如musl）更倾向于用这个来获取纳秒级的时间。
// 我们暂时不支持纳秒级时间，所以和do_gettimeofday差不多操作就行
static int32_t do_clock_gettime(struct intr_stack* stack) {
    // uint32_t clk_id = ARG1(stack); // CLOCK_REALTIME 等
    struct linux_timespec {
        uint32_t tv_sec;
        uint32_t tv_nsec;
    } *ts = (struct linux_timespec*)ARG2(stack);

    if (ts) {
        ts->tv_sec = sys_time();
        ts->tv_nsec = 0;
    }
    return 0;
}

static int32_t do_lstat64(struct intr_stack* stack){
     // 通过路径来获取文件信息
    const char* path = (const char*)ARG1(stack);
    // printk("[SYSCALL lstat64] ash is looking at: %s\n", path);
    struct linux_stat64* linux_buf = (struct linux_stat64*)ARG2(stack);
    
    return linux_sys_stat64(path, linux_buf,false);
}

static int32_t do_rt_sigaction(struct intr_stack* stack) {
    return sys_rt_sigaction(
        (int)ARG1(stack),
        (const struct sigaction*)ARG2(stack),
        (struct sigaction*)ARG3(stack),
        (uint32_t)ARG4(stack)
    );
}

static int32_t do_getuid32(struct intr_stack* stack UNUSED) {
    // 目前我们系统默认都是 root (UID 0)
    return 0;
}

static int32_t do_getppid(struct intr_stack* stack UNUSED) {
    return sys_getppid();
}

static int32_t do_getcwd(struct intr_stack* stack) {
    return (int32_t)sys_getcwd(
        (char*)ARG1(stack),
        (uint32_t)ARG2(stack)
    );
}

static int32_t do_geteuid32(struct intr_stack* stack UNUSED) {
    // 我们还没实现多用户权限
    // 默认返回 0 (root) 是最稳妥的，能让 ash 获得最高权限跑起来。
    return 0;
}
static int32_t do_rt_sigprocmask(struct intr_stack* stack) {
    int how = (int)ARG1(stack);
    const uint32_t* set = (const uint32_t*)ARG2(stack);
    uint32_t* oldset = (uint32_t*)ARG3(stack);
    uint32_t sigsetsize = ARG4(stack);

    // 校验 size (Linux 通常传 8 或 4)
    if (sigsetsize != 8 && sigsetsize != 4) return -EINVAL;

    return sys_sigprocmask(how, set, oldset);
}

static int32_t do_fork(struct intr_stack* stack UNUSED) {
    return sys_fork();
}

static int32_t do_wait4(struct intr_stack* stack) {
    pid_t pid      = (pid_t)ARG1(stack);    // 等待的目标 PID
    int32_t* status = (int32_t*)ARG2(stack); // 存放退出状态的指针
    int32_t options = (int32_t)ARG3(stack); // 选项，如 WNOHANG
    // struct rusage* usage = (void*)ARG4(stack); // 资源统计，暂时忽略
    return sys_waitpid(pid, status, options);
}

static int32_t do_execve(struct intr_stack* stack) {
    const char* filename = (const char*)ARG1(stack);
    const char** argv    = (const char**)ARG2(stack);
    const char** envp    = (const char**)ARG3(stack);
    
    return sys_execve(filename, argv, envp);
}

static int32_t do_setpgid(struct intr_stack* stack) {
    pid_t pid  = (pid_t)ARG1(stack);
    pid_t pgid = (pid_t)ARG2(stack);
    return sys_setpgid(pid, pgid);
}

static int32_t do_getpgid(struct intr_stack* stack) {
    pid_t pid = (pid_t)ARG1(stack);
    return sys_getpgid(pid);
}


// fcntl64 和 fcntl 用一个处理函数就行
// fcntl64 出现的历史原因是为了支持 大文件
// 它的锁结构体和普通版本的不同，当使用 F_GETLK64、F_SETLK64 等涉及文件锁的命令时
// fcntl 使用的是 struct flock，而 fcntl64 使用的是 struct flock64（其偏移量字段是 64 位的）。
// 由于我们的 sys_fcntl 目前还没有实现文件锁，仅仅处理了 FD 标志位和重定向
// 所以这种“结构体对齐”的区别暂时对我们没有影响。
static int32_t do_fcntl(struct intr_stack* stack) {
    int32_t fd = (int32_t)ARG1(stack);
    uint32_t cmd = (uint32_t)ARG2(stack);
    uint32_t arg = (uint32_t)ARG3(stack);

    switch (cmd) {
        case F_SETFL: {
            uint32_t kernel_arg = 0;
            // Linux i386: O_APPEND = 0x400, O_NONBLOCK = 0x800
            if (arg & 0x400) kernel_arg |= O_APPEND;   // 转为内置的 O_APPEND
            if (arg & 0x800) kernel_arg |= O_NONBLOCK; // 转为内置的 O_NONBLOCK
            
            // F_SETFL 只允许修改状态标志，读写模式是在 open 时确定的
            return sys_fcntl(fd, cmd, kernel_arg);
        }

        case F_GETFL: {
            // 调用内核获取当前的内核格式标志
            int32_t kflags = sys_fcntl(fd, cmd, arg);
            if (kflags < 0) return kflags; // 错误直接返回

            uint32_t linux_flags = 0;
            // 将内核标志转回 Linux 格式供用户态程序识别
            // 读写模式转换
            if (kflags & O_RDONLY) linux_flags |= 0;    // Linux O_RDONLY 是 0
            if (kflags & O_WRONLY) linux_flags |= 1; 
            if (kflags & O_RDWR)   linux_flags |= 2;

            // 状态标志转换
            if (kflags & O_APPEND)   linux_flags |= 0x400;
            if (kflags & O_NONBLOCK) linux_flags |= 0x800;
            if (kflags & O_CREATE)   linux_flags |= 0x40;
            if (kflags & O_TRUNC)    linux_flags |= 0x200;

            return (int32_t)linux_flags;
        }

        default:
            // 其他命令（如 F_DUPFD, F_GETFD 等）通常不涉及标志位转换，直接透传
            return sys_fcntl(fd, cmd, arg);
    }
}

static int32_t do_getpgrp(struct intr_stack* stack UNUSED) {
    // 根据 POSIX 和 Linux i386 标准，getpgrp() 不带参数
    // 它等价于我们的 sys_getpgid(0)，即获取当前线程的组id
    return sys_getpgid(0);
}

static int32_t do_kill(struct intr_stack* stack) {
    pid_t pid = (pid_t)ARG1(stack);
    int sig = (int)ARG2(stack);

    // 探测逻辑，Linux 中 sig=0 用于检查进程是否存在或是否有权限
    if (sig == 0) {
        struct task_struct* target = pid2thread(pid);
        return (target != NULL) ? 0 : -ESRCH; // ESRCH: 进程不存在
    }

    // 进程组逻辑，Linux 中如果 pid < -1，表示向进程组 |pid| 发送信号
    if (pid < -1) {
        kill_pgrp(-pid, sig);
        return 0;
    }

    // 广播逻辑，pid == -1 通常表示发给除 init 外的所有进程（我们先暂时不管它）
    // 当前进程组，pid == 0 表示发给调用者所在的进程组
    if (pid == 0) {
        kill_pgrp(get_running_task_struct()->pgrp, sig);
        return 0;
    }

    // 默认行为，普通的单进程发送
    int32_t ret = sys_kill(pid, sig);
    return (ret < 0) ? -ESRCH : 0;
}

static int32_t do_poll(struct intr_stack* stack) {
    struct pollfd* fds = (struct pollfd*)ARG1(stack);
    uint32_t nfds = (uint32_t)ARG2(stack);
    int32_t timeout_ms = (int32_t)ARG3(stack);

    // 调用你之前写好的那个核心逻辑函数
    return sys_poll(fds, nfds, timeout_ms);
}

static int32_t do_mkdir(struct intr_stack* stack) {
    // 按照 Linux i386 约定从寄存器获取参数
    const char* pathname = (const char*)stack->ebx;
    // uint32_t mode = (uint32_t)stack->ecx;

    // 参数校验
    if (pathname == NULL) {
        return -EFAULT;
    }

    // 我们的 sys_mkdir 权限目前内部写死了 0777，
    int32_t ret = sys_mkdir(pathname);

    // 如果 sys_mkdir 返回的是负值（错误码），直接返回
    // 否则返回 0 表示成功
    return ret;
}

static int32_t do_dup2(struct intr_stack* stack) {
    int32_t oldfd = (int32_t)stack->ebx;
    int32_t newfd = (int32_t)stack->ecx;

    return sys_dup2(oldfd, newfd);
}

static int32_t musl_sys_readv(int32_t fd, const struct linux_iovec* iov, int32_t iovcnt) {
    if (iov == NULL || iovcnt < 0) {
        return -EINVAL;
    }

    int32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        uint32_t iov_len = iov[i].iov_len;
        if (iov_len == 0) {
            continue;
        }

        // 依次调用已经实现好的 sys_read
        int32_t read_bytes = sys_read(fd, iov[i].iov_base, iov_len);

        if (read_bytes < 0) {
            // 如果已经读到了部分数据，先返回已读到的字节数
            if (total > 0) {
                return total;
            }
            return read_bytes; // 否则返回错误码
        }

        total += read_bytes;

        // 如果读到的字节数少于当前 iovec 要求的字节数
        // 说明数据读完了（EOF）或者目前管道/TTY 没数据了，直接返回
        if ((uint32_t)read_bytes < iov_len) {
            break;
        }
    }

    return total;
}

// readv 的作用是：一次性将文件中的数据读取并分发到多个互不连续的内存缓冲区中。
// readv 和 writev 的好处在于原子性和性能比较好，在某些情况下，多个缓冲区的操作是原子的。
// 并且可以减少系统调用的次数。比如 ash 想读一个头部信息到一个结构体，读后续数据到另一个缓冲区，用 readv 只需要进出内核一次。
static int32_t do_readv(struct intr_stack* stack) {
    return musl_sys_readv((int32_t)ARG1(stack),
                          (const struct linux_iovec*)ARG2(stack),
                          (int32_t)ARG3(stack));
}

static int32_t do_chdir(struct intr_stack* stack) {
    // 根据 Linux i386 约定，EBX 是路径字符串的指针
    const char* path = (const char*)stack->ebx;

    if (path == NULL) {
        return -EFAULT;
    }

    return sys_chdir(path);
}

static int32_t do_symlink(struct intr_stack* stack) {
    // ARG1: target (指向谁), ARG2: linkpath (新生成的链接名)
    const char* target = (const char*)ARG1(stack);
    const char* linkpath = (const char*)ARG2(stack);

    if (target == NULL || linkpath == NULL) {
        return -EFAULT;
    }

    return sys_symlink(target, linkpath);
}

static int32_t do_readlink(struct intr_stack* stack) {
    // ARG1: path, ARG2: buf, ARG3: bufsize
    const char* path = (const char*)ARG1(stack);
    char* buf = (char*)ARG2(stack);
    int32_t bufsize = (int32_t)ARG3(stack);

    if (path == NULL || buf == NULL) {
        return -EFAULT;
    }

    return sys_readlink(path, buf, bufsize);
}

static int32_t do_access(struct intr_stack* stack) {
    const char* pathname = (const char*)ARG1(stack);
    // int mode = (int)ARG2(stack); // 虽然我们暂不校验 mode，但参数在这

    if (pathname == NULL) return -EFAULT;

    return sys_access(pathname, 0); // 暂时忽略具体的 R/W/X mode
}

static int32_t do_rmdir(struct intr_stack* stack) {
    // 根据 i386 ABI，EBX (ARG1) 是路径字符串指针
    const char* pathname = (const char*)stack->ebx;

    if (pathname == NULL) {
        return -EFAULT;
    }

    return sys_rmdir(pathname);
}

static int32_t do_pipe(struct intr_stack* stack) {
    int32_t* user_pipefd = (int32_t*)ARG1(stack);
    // 必须确保 user_pipefd 在用户空间是可写的，否则会引发内核页错误
    // 简单的做法是先校验指针
    if (!user_pipefd) return -EFAULT;

    int32_t temp_fds[2];
    int32_t res = sys_pipe(temp_fds);
    
    if (res == 0) {
        // 把结果搬运回用户态
        user_pipefd[0] = temp_fds[0];
        user_pipefd[1] = temp_fds[1];
        return 0;
    }
    return res; // 返回具体的负数错误码
}

static int32_t do_rename(struct intr_stack* stack) {
    // 从寄存器中提取参数
    const char* oldpath = (const char*)stack->ebx;
    const char* newpath = (const char*)stack->ecx;

    // 基本的指针校验
    if (oldpath == NULL || newpath == NULL) {
        return -EFAULT;
    }

    return sys_rename(oldpath, newpath);
}

void musl_syscall_intrcpt_init(){
    for (int i = 0; i < NR_syscalls; i++) {
        musl_syscall_table[i] = do_default;
    }

    musl_syscall_table[__NR_getpid] = do_getpid;
    musl_syscall_table[__NR_exit_group] = do_exit_default;
    musl_syscall_table[__NR_exit] = do_exit_default;
    musl_syscall_table[__NR_writev] = do_writev;
    musl_syscall_table[__NR_ioctl] = do_ioctl;
    musl_syscall_table[__NR_brk] = do_brk;
    musl_syscall_table[__NR_munmap] = do_munmap;
    musl_syscall_table[__NR_madvise] = do_madvise;
    musl_syscall_table[__NR_open] = do_open;
    musl_syscall_table[__NR_write] = do_write;
    musl_syscall_table[__NR__llseek] = do_llseek;
    musl_syscall_table[__NR_read] = do_read;
    musl_syscall_table[__NR_close] = do_close;
    musl_syscall_table[__NR_stat64] = do_stat64;
    musl_syscall_table[__NR_fstat64] = do_fstat64;
    musl_syscall_table[__NR_getdents64] = do_getdents64;
    musl_syscall_table[__NR_unlink] = do_unlink;
    musl_syscall_table[__NR_time] = do_time;
    musl_syscall_table[__NR_mmap2] = do_mmap2;
    musl_syscall_table[__NR_gettimeofday] = do_gettimeofday;
    musl_syscall_table[__NR_clock_gettime] = do_clock_gettime;
    musl_syscall_table[__NR_lstat64] = do_lstat64;
    musl_syscall_table[__NR_rt_sigaction] = do_rt_sigaction; 
    musl_syscall_table[__NR_getuid32] = do_getuid32;     
    musl_syscall_table[__NR_getppid] = do_getppid;    
    musl_syscall_table[__NR_getcwd] = do_getcwd;      
    musl_syscall_table[__NR_geteuid32] = do_geteuid32;      
    musl_syscall_table[__NR_rt_sigprocmask] = do_rt_sigprocmask;      
    musl_syscall_table[__NR_fork] = do_fork;      
    musl_syscall_table[__NR_wait4] = do_wait4;      
    musl_syscall_table[__NR_execve] = do_execve;      
    musl_syscall_table[__NR_setpgid] = do_setpgid;
    musl_syscall_table[__NR_getpgid] = do_getpgid;
    musl_syscall_table[__NR_fcntl] = do_fcntl;
    musl_syscall_table[__NR_fcntl64] = do_fcntl;
    musl_syscall_table[__NR_getpgrp] = do_getpgrp;
    musl_syscall_table[__NR_kill] = do_kill;
    musl_syscall_table[__NR_poll] = do_poll;
    musl_syscall_table[__NR_mkdir] = do_mkdir;
    musl_syscall_table[__NR_dup2] = do_dup2;
    musl_syscall_table[__NR_readv] = do_readv;
    musl_syscall_table[__NR_chdir] = do_chdir;
    musl_syscall_table[__NR_symlink] = do_symlink; 
    musl_syscall_table[__NR_readlink] = do_readlink; 
    musl_syscall_table[__NR_access] = do_access; 
    musl_syscall_table[__NR_rmdir] = do_rmdir;
    musl_syscall_table[__NR_pipe] = do_pipe;
    musl_syscall_table[__NR_rename] = do_rename;
}

// 根据 i386 Linux ABI:
// eax: 调用号
// ebx: arg1, ecx: arg2, edx: arg3, esi: arg4, edi: arg5, ebp: arg6
void musl_syscall_interceptor(struct intr_stack* stack) {
    uint32_t sc_nr = stack->eax;
    int32_t ret = -ENOSYS; // 默认返回错误码

#ifdef DEBUG_SYSCALL_INTRCPT
    printk("Intrcpt 0x80: Syscall 0x%x from EIP: 0x%x\n", stack->eax, stack->eip);
#endif

    // 防止数组越界访问导致内核崩溃
    if (sc_nr >= NR_syscalls) {
        stack->eax = -ENOSYS;
        return;
    }
 
    ret = musl_syscall_table[sc_nr](stack);

    // 将返回值写入中断栈中的 eax 位置
    // 当汇编执行 popad 时，这个值会被加载到用户的 EAX 寄存器中
    // 按照 ABI 规定，系统调用只允许修改 EAX（作为返回值）
    // 其他的寄存器（EBX, ECX, EDX, ESI, EDI, EBP）在从系统调用返回后，必须保持和进入前一模一样。
    stack->eax = ret; 
}
