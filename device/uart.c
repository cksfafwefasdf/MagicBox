#include <uart.h>
#include <io.h>
#include <stdbool.h>
#include <interrupt.h>
#include <tty.h>

#define COM1 0x3F8

// 状态标志，防止在没有串口的机器上陷入死循环
static bool uart_present = false;

static void uart_interrupt_handler(void);


// 为了简单起见，目前先采用 轮询发送 + 中断接收 的方式
void uart_init() {
    // 关闭所有串口中断
    outb(COM1 + 1, 0x00); 

    // 硬件探测 (Loopback Test) 
    // 设置 MCR 寄存器为回环模式 (Bit 4 = 1)
    // 同时设置 RTS/DTR (Bit 0,1 = 1) 
    outb(COM1 + 4, 0x1E); 
    
    // 往数据寄存器写一个测试值
    outb(COM1 + 0, 0xAE);
    
    // 检查读回的值是否一致
    if (inb(COM1 + 0) != 0xAE) {
        uart_present = false;
        return; // 探测失败，直接返回
    }

    // 探测成功，将串口恢复为正常运行模式
    // 设置 Bit 4 = 0 关闭回环，开启 Bit 3 (OUT2) 用于触发中断
    outb(COM1 + 4, 0x0F); 
    uart_present = true;

    // 开启 DLAB (设置波特率)
    outb(COM1 + 3, 0x80); 
    
    // 设置波特率为 38400 (115200 / 3 = 38400)
    outb(COM1 + 0, 0x03); // 低 8 位
    outb(COM1 + 1, 0x00); // 高 8 位
    
    // 8位数据, 无校验, 1位停止位 (同时关闭了 DLAB)
    outb(COM1 + 3, 0x03); 
    
    // 启用并重置 FIFO (14字节触发)
    outb(COM1 + 2, 0xC7); 

    // 开启中断允许寄存器 (IER)
    // Bit 0: 接收数据可用中断 (Received Data Available Interrupt)
    outb(COM1 + 1, 0x01); 

    // MCR 寄存器：必须开启 Bit 3 (OUT2)，否则中断信号无法传递到中断控制器 (PIC)
    outb(COM1 + 4, 0x0B);

    register_handler(0x24,uart_interrupt_handler);
}

int is_transmit_empty() {
    if (!uart_present) return 1; // 如果硬件不存在，假装它是空的，防止阻塞
    return inb(COM1 + 5) & 0x20;
}

void uart_putc(char a) {
    if (!uart_present) return;
    while (is_transmit_empty() == 0); 
    outb(COM1, a);
}

void uart_puts(const char* s) {
    if (!uart_present) return;
    
    while (*s) {
        // 串口标准，遇到换行符，先补一个回车符 \r
        if (*s == '\n') {
            while (!(inb(COM1 + 5) & 0x20));
            outb(COM1, '\r');
        }
        while (!(inb(COM1 + 5) & 0x20));
        outb(COM1, *s++);
    }
}

int serial_received() {
    if (!uart_present) return 0;
    return inb(COM1 + 5) & 1;
}

char uart_getc() {
    if (!uart_present) return 0;
    while (serial_received() == 0); 
    return inb(COM1);
}

// 供外部查询状态
bool is_uart_present() {
    return uart_present;
}

static void uart_interrupt_handler(void) {
    // 检查 Line Status Register，确认确实有数据可读
    while (inb(COM1 + 5) & 1) {
        uint8_t c = inb(COM1);
        
        // 串口传来的回车通常是 \r (13)，
        // 而我们的 tty_input_handler 预期的是 \n 或处理 \r
        // 如果发现敲回车没反应，可以在这里做个简单的转换
        if (c == '\r') c = '\n'; 
        
        // 强制把 DEL 映射为 Backspace
        // 防止按下删除键 Backspace 没反应
        if (c == 0x7F) c = '\b'; 

        // 直接调 tty 的处理逻辑
        // 它会自动处理回显(ECHO)、退格、信号(SIGINT)等
        tty_input_handler(c);
    }
}