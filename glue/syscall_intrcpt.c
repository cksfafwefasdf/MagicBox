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

static int32_t linux_sys_stat64(const char* pathname, struct linux_stat64* buf) {
    struct stat kstat; // MagicBox 内核定义的 stat
    
    // 调用 MagicBox 原生的 sys_stat 获取数据
    int32_t ret = sys_stat(pathname, &kstat);
    if (ret < 0) return ret;

    // 格式转换，从 stat (MagicBox) 搬运到 linux_stat64 (Linux ABI)
    memset(buf, 0, sizeof(struct linux_stat64));
        buf->st_dev = kstat.st_dev;
    buf->st_ino = kstat.st_ino; // 低位 Inode
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

// 根据 i386 Linux ABI:
// eax: 调用号
// ebx: arg1, ecx: arg2, edx: arg3, esi: arg4, edi: arg5, ebp: arg6
void musl_syscall_interceptor(struct intr_stack* stack) {
    uint32_t m_eax = stack->eax;
    int32_t ret = -ENOSYS; // 默认返回错误码
#ifdef DEBUG_SYSCALL_INTRCPT
    printk("Intrcpt 0x80: Syscall 0x%x from EIP: 0x%x\n", stack->eax, stack->eip);
#endif
    switch (m_eax) {
        case __NR_getpid: // SYS_getpid
            ret = sys_getpid();
            break;
        // exit_group 会杀死一个进程的所有线程
        // 由于我们现在的用户进程都只对应一个内核线程，因此直接用exit就行
        case __NR_exit_group: 
        case __NR_exit:

            printk("Process %d exiting with status %d\n", 
            get_running_task_struct()->pid, 
            stack->ebx); // exit(status) 的参数在 ebx 里 

            sys_exit(stack->ebx);
            break;
        case __NR_writev:
            ret = musl_sys_writev((int32_t)stack->ebx,
                                  (const struct linux_iovec*)stack->ecx,
                                  (int32_t)stack->edx);
            break;
        case __NR_ioctl:
            ret = sys_ioctl((int32_t)stack->ebx,
                            (uint32_t)stack->ecx,
                            (uint32_t)stack->edx);
            break;
        case __NR_brk:
            ret = (int32_t)sys_brk((uint32_t)stack->ebx);
            break;
        case __NR_mmap2:
            ret = (int32_t)musl_sys_mmap2((uint32_t)stack->ebx,
                                          (uint32_t)stack->ecx,
                                          (uint32_t)stack->edx,
                                          (uint32_t)stack->esi,
                                          (int32_t)stack->edi,
                                          (uint32_t)stack->ebp);
            break;
        case __NR_munmap:
            ret = sys_munmap((uint32_t)stack->ebx,
                             (uint32_t)stack->ecx);
            break;
        
        case __NR_madvise: // 这是一个malloc的性能优化函数，不是必须的，为了防止报unknown警告，我直接no-op返回
            ret = 0;
            break;
        
        // 较老版本的编译器中只能在语句块中定义变量
        // 我们这个open里面定义了path，依次加个花括号给他括住
        case __NR_open:{
            const char* path = (const char*)stack->ebx;
            uint32_t linux_flags = stack->ecx;
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

            ret = sys_open(path, kernel_flags);
            break;
        }

        case __NR_write: // 0x4
            // EBX: fd, ECX: buf, EDX: count
            ret = sys_write((int32_t)stack->ebx, 
                            (void*)stack->ecx, 
                            (uint32_t)stack->edx);
            break;
        
        case __NR__llseek:{
            printk("Intrcpt: warning, use lseek as llseek\n");
            int32_t fd = (int32_t)stack->ebx;
            int32_t offset = (int32_t)stack->edx; // 取低32位
            int64_t* res_ptr = (int64_t*)stack->esi;
            
            // 对齐 whence: Linux(0,1,2) -> 我们的(1,2,3)
            uint32_t linux_whence = stack->edi;
            uint32_t kernel_whence = linux_whence + 1;

            // 边界检查，防止用户传入非法值导致 ASSERT 触发
            if (kernel_whence < 1 || kernel_whence > 3) {
                ret = -EINVAL;
                break;
            }

            int32_t new_pos = sys_lseek(fd, offset, kernel_whence);

            if (new_pos < 0) {
                ret = new_pos; // 此时 ret 为 -1 或错误码
            } else {
                if (res_ptr != NULL) {
                    *res_ptr = (int64_t)new_pos;
                }
                ret = 0; // 成功返回 0
            }
            break;
        }

        case __NR_read: {
            // EBX: fd, ECX: buf, EDX: count
            int32_t fd = (int32_t)stack->ebx;
            void* buf = (void*)stack->ecx;
            uint32_t count = (uint32_t)stack->edx;

            ret = sys_read(fd, buf, count);
            break;
        }

        case __NR_close: {
            // EBX: fd
            int32_t fd = (int32_t)stack->ebx;

            ret = sys_close(fd);
            break;
        }

        case __NR_stat64: { // 通过路径来获取文件信息
            const char* path = (const char*)stack->ebx;
            struct linux_stat64* linux_buf = (struct linux_stat64*)stack->ecx;
            
            ret = linux_sys_stat64(path, linux_buf);
            break;
        }

        case __NR_fstat64: { // 通过描述符fd来获取文件信息
            int32_t fd = (int32_t)stack->ebx;
            struct linux_stat64* linux_buf = (struct linux_stat64*)stack->ecx;
            
            ret = linux_sys_fstat64(fd, linux_buf);
            break;
        }

        case __NR_getdents64: // 用户态的 readdir 函数底层会进行这个调用
            ret = linux_sys_getdents64((int32_t)stack->ebx, (void*)stack->ecx, (uint32_t)stack->edx);
            break;

        case __NR_unlink: { 
            const char* path = (const char*)stack->ebx;
            ret = sys_unlink(path);
            break;
        }

        case __NR_time: {
            uint32_t* tloc = (uint32_t*)stack->ebx;
            int32_t now = sys_time(); // 返回 Unix 秒数
            if (tloc) *tloc = now;
            ret = now;
            break;
        }

        default:
            printk("Unknown Syscall 0x%x\n", m_eax);
            // 按照 Linux 规范，未实现的调用通常返回 -38 (ENOSYS)
            ret = -ENOSYS; 
            break;
    }

    // 将返回值写入中断栈中的 eax 位置
    // 当汇编执行 popad 时，这个值会被加载到用户的 EAX 寄存器中
    // 按照 ABI 规定，系统调用只允许修改 EAX（作为返回值）
    // 其他的寄存器（EBX, ECX, EDX, ESI, EDI, EBP）在从系统调用返回后，必须保持和进入前一模一样。
    stack->eax = ret; 
}
