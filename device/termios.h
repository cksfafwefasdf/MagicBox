#ifndef __DEVICE_TERMIOS_H
#define __DEVICE_TERMIOS_H

// c_lflag (本地模式) 的位掩码
#define ISIG 0x0001 // 启用信号处理（若收到 INTR, QUIT, SUSP 字符，则发送信号）
#define ICANON 0x0002 // 启用规范模式（按行缓冲，允许退格编辑）
#define ECHO 0x0008 // 启用输入回显

// c_cc 数组的索引（控制字符） 
#define VINTR 0 // c_cc[0] 对应 Ctrl+C (SIGINT)
#define VQUIT 1 // c_cc[1] 对应 Ctrl+\ (SIGQUIT)
#define VERASE 2 // c_cc[2] 对应退格 (Backspace)
#define VKILL 3 // c_cc[3] 对应 Ctrl+U (删掉整行)
#define VEOF 4 // c_cc[4] 对应 Ctrl+D (文件结束符)
#define VCLR 5 // c_cc[5] 对应 Ctrl+L (清屏)

/* 新增 ioctl 命令码 */
#define TCGETS 0x5401  // 获取 termios
#define TCSETS 0x5402  // 设置 termios

#define TIOCSPGRP 0x5410
#define TIOCGPGRP 0x540F

#define NCC 8

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[NCC]; // 记录特殊按键，如 c_cc[VINTR] = 0x03 (Ctrl+C)
};

#endif