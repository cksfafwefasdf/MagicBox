#ifndef __INCLUDE_MAGICBOX_IDE_DMA_H
#define __INCLUDE_MAGICBOX_IDE_DMA_H

#include <stdint.h>

struct disk;

#define PRD_EOT 0x8000  // End of Table: 1000 0000 0000 0000

#define PORT_NUM 8

// IDE Bus Master 寄存器偏移 (相对于 BMBA)
#define BM_COMMAND_REG_OFFSE        0x00
#define BM_STATUS_REG_OFFSE         0x02
#define BM_PRDT_ADDR_REG_OFFSET     0x04

// BM_COMMAND 寄存器位
#define BM_CMD_START         0x01  // 1=开始传输, 0=停止
#define BM_CMD_READ          0x08  // 1=硬盘到内存(读), 0=内存到硬盘(写)

// BM_STATUS 寄存器位
#define BM_STATUS_ACTIVE     0x01  // 传输激活中
#define BM_STATUS_ERROR      0x02  // 传输过程中发生错误
#define BM_STATUS_INT        0x04  // 传输完成产生中断 (写 1 清除) 

// prdt （Physical Region Descriptor Table） 相当于是一个任务清单
// 每一个 PRD 条目都会告诉 DMA 控制器去哪搬（数据的物理内存地址）、搬多少（字节数）、是不是最后一件（EOT 标志）
// DMA 控制器不会主动去扫描内存里的 PRDT
// 只有当 CPU 向 BMBA 寄存器（Command 端口） 写入“开始”位（Bit 0 = 1）时，它才会去 PRDT 指向的地址读第一行任务
// 一旦它读到了带有 EOT (End of Table) 标志的最后一行，并且搬运完了数据，它就会立即停止，并向 CPU 发出一个中断信号，然后原地待命。
struct prd {
    uint32_t paddr; // 数据的物理地址
    uint16_t size; // 传输字节数，如果 size 填 0，它代表传输 65536 字节 (64KB)。这和我们 pio 的时候类似。
    uint16_t flags; // 最高位 bit 15 是 EOT (End of Table)
};

extern void ide_pci_driver_init(void);
extern void ide_read_dma(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);
extern void ide_write_dma(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt);

#endif