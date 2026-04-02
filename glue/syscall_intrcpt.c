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

// 根据 i386 Linux ABI:
// eax: 调用号
// ebx: arg1, ecx: arg2, edx: arg3, esi: arg4, edi: arg5, ebp: arg6
void musl_syscall_interceptor(struct intr_stack* stack) {
    uint32_t m_eax = stack->eax;
    int32_t ret = -ENOSYS; // 默认返回错误码
#ifdef DEBUG_SYSCALL_INTRCPT
    // printk("Intrcpt 0x80: Syscall 0x%x from EIP: 0x%x\n", stack->eax, stack->eip);
#endif
    switch (m_eax) {
        case __NR_getpid: // SYS_getpid
            ret = sys_getpid();
#ifdef DEBUG_SYSCALL_INTRCPT
            printk("PID requested: %d\n", ret);
#endif
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
