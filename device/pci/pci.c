#include <pci.h>
#include <io.h>
#include <dlist.h>
#include <stdio-kernel.h>
#include <stdint.h>
#include <stdbool.h>
#include <ide_dma.h>
#include <memory.h>


struct dlist pci_drivers_list; // 全局已注册驱动链表

/*
32 位地址格式：
    位 (Bits)       宽度        含义    
    31              1 bit       使能位 (Enable)，如果这一位是 0，PCI 控制器就会忽略后续的操作
    30-24           7 bits      保留
    23-16           8 bits      总线号 (Bus)
    15-11           5 bits      设备号 (Device/Slot)
    10-8            3 bits      功能号 (Function)
    7-2             6 bits      寄存器偏移 (Offset) 
    1-0             2 bits      必须为 0,
*/

static bool pci_match_id(const struct pci_device_id* id, struct pci_dev* dev) {
    if (id->vendor_id != 0xFFFFFFFF && id->vendor_id != dev->vendor_id) return false;
    if (id->device_id != 0xFFFFFFFF && id->device_id != dev->device_id) return false;
    if ((dev->class_code & id->class_mask) != id->class_code) return false;
    return true;
}

// 为发现的设备寻找驱动
void pci_bind_driver(struct pci_dev* dev) {
    struct dlist_elem* it = pci_drivers_list.head.next;

    while (it != &pci_drivers_list.tail) {
        
        struct pci_driver* drv = member_to_entry(struct pci_driver , driver_tag, it);
        
        const struct pci_device_id* id = drv->id_table;
        
        // 匹配该驱动支持的所有 ID 条目
        // 如果 id 条目全是 0 则表示 table 结束
        while (id->vendor_id != 0 || id->class_code != 0) {
            if (pci_match_id(id, dev)) {
                printk("PCI: Bind [%s] to Device 0x%x:0x%x\n", 
                        drv->name, dev->vendor_id, dev->device_id);

                if (drv->probe(dev) == 0) {
                    return; // 成功绑定并初始化，退出
                } else {
                    printk("PCI: Driver [%s] probe failed.\n", drv->name);
                }
            }
            id++; 
        }
        
        it = it->next; 
    }
}

// 读取 PCI 配置空间的一个 32 位数据
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    // 构造 32 位的地址
    // 所有的 PCI 设备都至少要有 256 字节的配置空间，前 64 个字节是标准化的（可以参考 LDD 的 306 页）
    // 其余是设备相关的，offset 表示我们要读 256 字节中的哪一部分的数据
    // 一次可以读出 32 字节
    // PCI 配置空间的读写必须是 4 字节对齐的
    // 即便想读偏移为 0x01 的字节，也必须先读出偏移为 0x00 的整个 32 位双字，然后再通过位运算把需要的那 8 位抠出来。
    uint32_t address = (uint32_t)((1 << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xfc));
    // out 之后，PCI 控制器会在后台根据地址，把对应设备的数据准备好放在 0xCFC 寄存器上
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

// 写入 PCI 配置空间的一个 32 位数据
// 具体的过程和 pci_read_config 基本一致
void pci_write_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((1 << 31) | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xfc));
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, val);
}


void pci_check_device(uint8_t bus, uint8_t slot) {
    uint8_t func = 0;
    // 提取第 2 字节，他们自低向高分别是厂商 ID 和 设备 ID
    // 如果 Vendor ID 是 0xFFFF，说明该位置没有物理硬件响应
    uint32_t vendor_id = pci_read_config(bus, slot, 0, PCI_VENDOR_OFFSET);
    if ((vendor_id & 0xFFFF) == 0xFFFF) return; 

    for (func = 0; func < PCI_FUNC_NUM; func++) {
        
        // 探测功能是否存在
        uint32_t data = pci_read_config(bus, slot, func, PCI_VENDOR_OFFSET);
        if ((data & 0xFFFF) == 0xFFFF) continue;
        
        struct pci_dev* dev = kmalloc(sizeof(struct pci_dev));
        dev->bus = bus;
        dev->slot = slot;
        dev->func = func;
        dev->vendor_id = data & 0xFFFF;
        dev->device_id = data >> 16;

        // 偏移 8 字节处读到的是类代号，他是一个 16 位的数据，高 8 位标识了 base class
        // 例如 ethernet 和 token ring 都属于 network 基类
        // serial 和 parallel 同属 communication（通信）组
        // 偏移 0x08 处的 32 位总体上是：
        // [31:24] Base Class (大类)
        // [23:16] Sub Class  (子类)
        // [15:8]  Prog IF    (编程接口)
        // [7:0]   Revision   (版本号)
        uint32_t rev_class = pci_read_config(bus, slot, func, 0x08);
        uint8_t base_class = (rev_class >> 24) & 0xFF;
        uint8_t sub_class  = (rev_class >> 16) & 0xFF;
        uint8_t prog_if    = (rev_class >> 8) & 0xFF;
        // class_code 包含 Base, Sub, ProgIF
        dev->class_code = (base_class << 16) | (sub_class << 8) | prog_if;
        printk("PCI Dev: 0x%x:0x%x Class: 0x%x\n", dev->vendor_id, dev->device_id, dev->class_code);
        // 匹配驱动
        pci_bind_driver(dev); 
    }
}

// pci_init 在 ide_init 之后调用，它里面有 dma 的初始化逻辑，他作为我们磁盘 io 的一个增强包
// 如果可以 dma，那么就用dma，不可以那就还用 pio
void pci_init() {
    printk("PCI init start\n");
    dlist_init(&pci_drivers_list);

    // 注册所有已知的驱动程序
    // 将 ide_driver, net_driver 等 push 到 pci_drivers_list 
    // 我们目前的设备还不是很多，先这么硬编码初始化逻辑吧
    // 以后如果设备多了再实现从文件系统中运行时加载
    ide_pci_driver_init(); 
    // network_pci_driver_init();

    // 扫描总线
    printk("PCI Scanning...\n");
    for (uint16_t bus = 0; bus < PCI_BUS_NUM; bus++) {
        for (uint8_t slot = 0; slot < PCI_SLOT_NUM; slot++) {
            pci_check_device(bus, slot);
        }
    }
    printk("PCI Scanning done\n");
    printk("PCI init done\n");
}