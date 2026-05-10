#ifndef __INCLUDE_MAGICBOX_PCI_H
#define __INCLUDE_MAGICBOX_PCI_H

#include <stdint.h>
#include <dlist.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// PCI 配置空间的各个字段的偏移
#define PCI_VENDOR_OFFSET       0x00
#define PCI_DEVICE_OFFSET       0x02
#define PCI_CMD_REG_OFFSET      0x04
#define PCI_STATUS_REG_OFFSET   0x06
#define PCI_CLASS_OFFSET        0x08
#define PCI_BAR0_OFFSET         0x10
#define PCI_BAR1_OFFSET         0x14
#define PCI_BAR2_OFFSET         0x18
#define PCI_BAR3_OFFSET         0x1c
#define PCI_BAR4_OFFSET         0x20
#define PCI_BAR5_OFFSET         0x24

// PCI COMMAND 寄存器位掩码
#define PCI_COMMAND_IO          0x01  // 允许 I/O 访问
#define PCI_COMMAND_MEMORY      0x02  // 允许内存访问
#define PCI_COMMAND_MASTER      0x04  // 允许 Bus Master，将这位置为 1 就可以允许窃取 CPU 的总线周期
#define PCI_COMMAND_INTERRUPT_DISABLE 0x400 // 禁用中断

// PCI 协议规定每个系统支持 256 条总线，每条总线 32 个设备/插槽 (slot)，每个设备最多 8 个功能。
// 虽然理论上有 256 条总线，但大多数个人电脑往往只有 Bus 0 有设备，或者只有少数几条总线。
// 例如 Bus 0, Slot 1, Func 1：在 QEMU 中，这通常就是 IDE 控制器。
// Bus 0, Slot 3, Func 0：通常是 网卡（如 RTL8139 或 E1000）。
#define PCI_BUS_NUM 256
#define PCI_SLOT_NUM 32
#define PCI_FUNC_NUM 8

// PCI 配置空间地址结构，忽略了一部分不必要的字段
struct pci_dev {
    uint8_t bus;        // 总线号 (0-255)
    uint8_t slot;       // 设备号 (0-31)
    // 一个物理硬件可能同时具备多个功能
    // 例如：一个集成的芯片组可能既是“磁盘控制器(Func 0)”，又是“USB 控制器(Func 1)”，还是“声卡(Func 2)”。
    uint8_t func;       // 功能号 (0-7)
    uint16_t vendor_id; // 厂商 ID
    uint16_t device_id; // 设备 ID
    // 分类码 (01h 代表存储, 02h 代表网络) 
    // 某些驱动程序可能可以支持多个不同签名（vendorID + deviceID）的设备，但同属一个类
    uint32_t class_code; // 包含 Base(24-16), Sub(15-8), ProgIF(7-0)
    uint8_t subclass;   // 子类码, 用于进一步区分设备
    uint32_t bar[6];    // 6个基地址寄存器 base address register
    uint32_t irq_line;  // 中断号
};

// ID 匹配表
struct pci_device_id {
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t class_code; // 例如 0x010100 (Base, Sub, Interface)
    uint32_t class_mask; // 屏蔽位，用于只匹配大类
};

struct pci_driver {
    char* name;
    const struct pci_device_id* id_table;
    
    // 当 ID 匹配成功时调用，相当于一个初始化函数
    int (*probe)(struct pci_dev* dev);
    // 当驱动卸载或设备拔出时调用
    void (*remove)(struct pci_dev* dev);
    
    struct dlist_elem driver_tag; // 用于挂载到全局驱动链表
};

extern void pci_bind_driver(struct pci_dev* dev);
extern uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
extern void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
extern void pci_check_device(uint8_t bus, uint8_t slot);
extern void pci_init(void);

extern struct dlist pci_drivers_list;

#endif