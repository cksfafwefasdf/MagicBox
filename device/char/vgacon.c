#include <vgacon.h>
#include <console.h>
#include <string.h>
#include <device.h>
#include <io.h>
#include <interrupt.h>

// 显存起始虚拟地址
// VGA 模式的显存的地址在硬件上就被映射在低端 1MB 物理内存的 0xb8000 处
// 这处于我们的低端内存中，内核可以直接通过访问地址 0xc00b8000 来访问这块物理内存
#define VIDEO_ADDR (uint16_t*)0xc00b8000  

#define CH_CR 0xd // carriage return
#define CH_LF 0xa // line feed
#define CH_BS 0x8 // backspace
#define CH_TAB 0x9 // tab
#define CH_SPACE 0x20
#define CH_STR_END 0x0
#define CH_ESC 0x1b
#define CH_BELL 0x07

#define COLOR_BLACK 0x00
#define COLOR_BLUE 0x0B
#define COLOR_GREEN 0x0A
#define COLOR_CYAN 0x03
#define COLOR_RED 0x0C
#define COLOR_MAGENTA 0x05
#define COLOR_YELLOW 0x0e
#define COLOR_WHITE 0x0f // 默认的输出风格为白色，亮白色

#define TAB_SPACE_SIZE 0x8

enum vgacon_status {
    NORMAL, // normal character
    ESC, // ESC
    BRACKET // '['
};

// ANSI 颜色对应 VGA 属性表
static uint8_t color_table[] = { COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE };

// 状态机全局变量
static enum vgacon_status out_status = NORMAL;
static uint16_t ansi_param_val = 0;
static uint16_t ansi_params[2] = {0, 0};
static int param_idx = 0;

// 引用汇编中定义的全局变量
extern void cls_screen(void); // 直接使用汇编实现的快速清屏
extern uint8_t current_char_style; // 与汇编共用一个主题变量

struct console_device console_vgacon;

static uint16_t get_cursor(void) {
    outb(0x3d4, 0x0e);
    uint16_t pos = inb(0x3d5) << 8;
    outb(0x3d4, 0x0f);
    pos |= inb(0x3d5);
    return pos;
}

static void set_hw_cursor(uint16_t pos) {
    outb(0x3d4, 0x0e);
    outb(0x3d5, (uint8_t)(pos >> 8));
    outb(0x3d4, 0x0f);
    outb(0x3d5, (uint8_t)(pos & 0xff));
}

void vgacon_init(){
    memset(&console_vgacon, 0, sizeof(struct console_device));
    console_vgacon.put_char = put_char;
    console_vgacon.put_str = put_str;
    console_vgacon.put_int = put_int;
    console_vgacon.rdev = MAKEDEV(TTY_MAJOR, TTY0_MINOR);
    memset(console_vgacon.name,0,CONSOLE_DEV_NAME_LEN);
    strcpy(console_vgacon.name,"tty0");
    console_register(&console_vgacon);
}

void put_char(char char_ascii) {
    // 关中断保证原子性
    enum intr_status old_status = intr_disable();

    uint16_t cursor_pos = get_cursor();
    volatile uint16_t* video_mem = (volatile uint16_t*)VIDEO_ADDR;

    if (out_status == NORMAL) { // Normal 模式

        switch (char_ascii) {
            case CH_ESC: // ESC
                out_status = ESC;
                ansi_param_val = 0;
                param_idx = 0;
                ansi_params[0] = ansi_params[1] = 0;
                break;
            case '\r': // Carriage Return
                cursor_pos -= (cursor_pos % SCREEN_WIDTH);
                break;
            case '\n': // Line Feed
                cursor_pos += SCREEN_WIDTH;
                break;
            case '\b': // Backspace
            // \b 只会移动光标不会删除数据，要删数据的话需要手动输出一个 "'\b',' ','\b'" 序列才行
            // 这是标准的处理方法
                if (cursor_pos > 0) {
                    cursor_pos--;
                }
                break;
            case '\t': // Tab 
                // 在我们之前的 vga 驱动中，tab 固定移动 8 个字符
                // 因此我们的排版经常会很乱，尤其是在输入 ps 命令时，因为虽然 tab 移动的距离是 8 确实不变
                // 但是我们的字符串长度却不总是一样，固定移动 8 的话在不同长度的字符串上看起来就会很乱
                // 而标准终端的 Tab 应该是移动到下一个 8 的倍数列。
                // 不是固定加 8，而是对齐到下一个 8 的倍数坐标
                // cursor_pos + 1 是为了保证 tab 总是能向后移动
                // 比如当光标位置要刚好是 8（或者 8 的倍数）的话，不加 1 会使得移动步长变成 0，从而不移动
                // 加个 1 的话 就可以补偿到 8，这是正常的行为
                // 即 cursor_pos = 8：DIV_ROUND_UP(9, 8) * 8 = 16，会将光标从原本的 8 移动到 16，移动了 8 个单位
                // 假如光标位置是 7 的话，移动后的位置是 DIV_ROUND_UP(8,8)*8 = 8，这会使得光标向后移动一个单位，符合预期
                uint16_t next_tab_stop = DIV_ROUND_UP(cursor_pos + 1, TAB_SPACE_SIZE) * TAB_SPACE_SIZE;
                while (cursor_pos < next_tab_stop && cursor_pos < SCREEN_WIDTH * SCREEN_HEIGHT) {
                    video_mem[cursor_pos++] = (uint16_t)((current_char_style << 8) | ' ');
                    // 需要注意的是 Tab 也要检查是否需要滚屏
                    if (cursor_pos >= SCREEN_WIDTH * SCREEN_HEIGHT) break;
                }
                break;
            case CH_BELL:
                // 这个字符是 "Visual Bell"（可见警报）
                // 在 Linux 终端中，当我们执行了一个“非法”或者“无结果”的操作时
                // 比如在没有更多补全建议时狂按 Tab，或者在行首按 Backspace
                // Shell 会触发一个警报（Bell）
                // 在标准 Bell 中，会发送 ASCII 码 0x07 (\a)。如果系统的驱动接了蜂鸣器，它会“哔”一声。
                // 如果我们不跳过他的话他会在屏幕上输出一个 · 来占位警告，这是正常的
                break;
            default: // 普通字符
                video_mem[cursor_pos++] = (uint16_t)((current_char_style << 8) | char_ascii);
                break;
        }

    } else if (out_status == ESC) { // 收到 ESC

        out_status = (char_ascii == '[') ? BRACKET : NORMAL;

    } else if (out_status == BRACKET) { // 收到 [，解析参数

        if (char_ascii >= '0' && char_ascii <= '9') {
            // 将传进来的 \e[0;31;40m 中的字符串 31 转换成整型的 31 数据，以便处理
            ansi_param_val = ansi_param_val * 10 + (char_ascii - '0');
        } else if (char_ascii == ';') {
            if (param_idx < 1) {
                ansi_params[param_idx++] = ansi_param_val;
                ansi_param_val = 0;
            }
        } else {
            // 参数解析结束，存入最后一个参数
            ansi_params[param_idx] = ansi_param_val;

            // 指令分发
            switch (char_ascii) {
                case 'H': case 'f': { // 光标定位
                    int row = (ansi_params[0] == 0) ? 0 : ansi_params[0] - 1;
                    int col = (ansi_params[1] == 0) ? 0 : ansi_params[1] - 1;
                    // 边界检查
                    if (row >= SCREEN_HEIGHT) row = SCREEN_HEIGHT - 1;
                    if (col >= SCREEN_WIDTH) col = SCREEN_WIDTH - 1;
                    cursor_pos = row * SCREEN_WIDTH + col;
                    break;
                }
                case 'm': { 
                    // ANSI 可以一次传多个参数，例如 \e[0;31;40m
                    // 我们至少要循环处理保存的两个参数
                    for (int i = 0; i <= param_idx; i++) {
                        uint16_t m_val = ansi_params[i];
                        
                        if (m_val == 0) {
                            current_char_style = COLOR_WHITE; // 重置为黑底白字
                        } 
                        else if (m_val >= 30 && m_val <= 37) {
                            current_char_style = color_table[m_val - 30]; // 设置前景
                        }
                        else if (m_val >= 40 && m_val <= 47) {
                            // 在 VGA 文本模式下，显存中每个字符占用 2 个字节
                            // 低 8 位是字符的 ASCII 码
                            // 高 8 位是属性字节（控制颜色，就是我们此处的 current_char_style）
                            // 属性字节的 8 位又进一步被划分为两部分
                            // 低 4 位 (0-3) 是前景色 (Foreground)
                            // 高 4 位 (4-7) 是背景色 (Background)
                            // 其中第 7 位通常控制闪烁（Blink），所以背景色通常只用到低 3 位。
                            // 此处我们只换背景，不换前景
                            current_char_style = (current_char_style & 0x0F) | ((color_table[m_val - 40] & 0x07) << 4);
                        }
                    }
                    break;
                }
                case 'J': {
                    // 标准 ANSI 中，J 默认是 0J，即清除光标到屏幕末尾
                    // 只有 2J 才是真正的全清屏
                    uint16_t j_type = ansi_params[0];
                    
                    if (j_type == 2) {
                        cls_screen(); // 全清屏
                        cursor_pos = 0;
                    } else {
                        // 处理 0J 或默认 J：从当前光标清到显存末尾
                        for (int i = cursor_pos; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
                            video_mem[i] = (uint16_t)((current_char_style << 8) | ' ');
                        }
                    }
                    break;
                }
                case 'K': { // 行内清除
                    int start = cursor_pos;
                    int end = (cursor_pos / SCREEN_WIDTH + 1) * SCREEN_WIDTH;
                    for (int i = start; i < end; i++) {
                        video_mem[i] = (uint16_t)((current_char_style << 8) | ' ');
                    }
                    break;
                }
                case 'A': { // Up
                    int count = (ansi_params[0] == 0) ? 1 : ansi_params[0];
                    while (count > 0) {
                        if (cursor_pos >= SCREEN_WIDTH) {
                            cursor_pos -= SCREEN_WIDTH;
                        } else {
                            // 如果已经在第一行，直接出栈
                            cursor_pos %= SCREEN_WIDTH; // 保持在第一行的当前列
                            break;
                        }
                        count--;
                    }
                    break;
                }
                case 'B': { // Down
                    int count = (ansi_params[0] == 0) ? 1 : ansi_params[0];
                    while(count-- && cursor_pos < SCREEN_WIDTH * (SCREEN_HEIGHT - 1)) cursor_pos += SCREEN_WIDTH;
                    break;
                }
                case 'C': { // Right
                    int count = (ansi_params[0] == 0) ? 1 : ansi_params[0];
                    while(count-- && cursor_pos < SCREEN_WIDTH * SCREEN_HEIGHT - 1) cursor_pos++;
                    break;
                }
                case 'D': { // 向左移动 (ESC [ D)
                    int count = (ansi_params[0] == 0) ? 1 : ansi_params[0];
                    // 不能跨行退回上一行，且不能退过 0
                    while (count > 0 && (cursor_pos % SCREEN_WIDTH) > 0) {
                        cursor_pos--;
                        count--;
                    }
                    break;
                }

            }
            // 指令处理完了，重置状态机
            out_status = 0;
            ansi_param_val = 0;
            ansi_params[0] = 0;
            ansi_params[1] = 0;
            param_idx = 0;
        }
    }

    // 滚屏处理
    if (cursor_pos >= SCREEN_WIDTH * SCREEN_HEIGHT) {
        // 使用 memcpy 替代 for 循环，这在底层等同于 rep movsd，性能最高
        // 移动前 24 行 (24 * 80 * 2 字节)
        memcpy((void*)VIDEO_ADDR, (void*)(VIDEO_ADDR + SCREEN_WIDTH), SCREEN_WIDTH * (SCREEN_HEIGHT - 1) * 2);
        
        // 清除最后一行
        for (int i = SCREEN_WIDTH * (SCREEN_HEIGHT - 1); i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
            video_mem[i] = (uint16_t)((current_char_style << 8) | ' ');
        }
        cursor_pos = SCREEN_WIDTH * (SCREEN_HEIGHT - 1);
    }

    if (cursor_pos >= SCREEN_WIDTH * SCREEN_HEIGHT) {
        // 如果是因为逻辑错误导致的溢出，将其重置到最后一行行首或当前行
        cursor_pos = SCREEN_WIDTH * (SCREEN_HEIGHT - 1);
    }

    // 更新硬件光标
    set_hw_cursor(cursor_pos);

    // 恢复中断状态 (对应汇编 popfd)
    intr_set_status(old_status);
}