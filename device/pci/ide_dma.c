#include <ide_dma.h>
#include <pci.h>
#include <stdint.h>
#include <ide.h>
#include <memory.h>
#include <io.h>
#include <stdio-kernel.h>
#include <debug.h>
#include <string.h>

static int ide_dma_probe(struct pci_dev* dev);

// 创建 ide 设备的 pci id 表，用于匹配设备
// 只有这个表格中存储的设备才能使用当前文件中定义的驱动
static const struct pci_device_id ide_pci_ids[] = {
    { 
        .vendor_id = 0xFFFFFFFF,  // 匹配所有厂商
        .device_id = 0xFFFFFFFF,  // 匹配所有型号
        .class_code = 0x010100,   // Base: 01 (Storage), Sub: 01 (IDE) 遮掉最后 8 位的 ProgIF
        .class_mask = 0xFFFF00    // 只比较前 16 位 (Base 和 Sub)
    },
    {0} // 以全 0 结尾，方便 bind 函数判断结束
};

static struct pci_driver ide_pci_driver = {
    .name = "IDE_DMA_Driver",
    .id_table = ide_pci_ids,
    .probe = ide_dma_probe,
    .remove = NULL, // remove 先空着
};

/*
    位 (Bit)    掩码 (Mask)     名称             作用
    0           0x01            I/O Space       允许设备响应 I/O 端口访问
    1           0x02            Memory Space    允许设备响应 MMIO 访问
    2           0x04            Bus Master      允许设备发起 DMA 传输（主控权限）
    3           0x08            Special Cycles  允许响应特殊周期
    4           0x10            MWI Enable      允许存储器写并无效命令
*/

static int ide_dma_probe(struct pci_dev* dev) {
    printk("IDE Driver: Probing device 0x%x:0x%x.%d\n", dev->bus, dev->slot, dev->func);

    // 获取并校验 BAR4 (DMA 控制寄存器的基地址)
    // 0x20 是 BAR4 的偏移
    uint32_t bar4 = pci_read_config(dev->bus, dev->slot, dev->func, PCI_BAR4_OFFSET);
    if (!(bar4 & PCI_COMMAND_IO)) { 
        // 检查最低位，BAR4 必须是 IO 映射
        // 否则的话我们就不能用 in 和 out 指令来访问了，得用内存指针操作来访问，就像操作 vga 显存那样
        // 通常按照规定来说，IDE DMA 都是 IO 映射的
        printk("IDE Warning: BAR4 is not I/O mapped, DMA might fail.\n");
        return -1;
    }

    // 每个通道（Channel）分配 8 个 8 位的 IO 端口。如果 bmba 是 0xC000，那么
    // 0xC000 (Command): 控制寄存器。设置读写方向，启动/停止 DMA。Read 为 0x08，Write 为 0x00
    // 0xC002 (Status): 状态寄存器。硬件告诉你传输是否成功、是否产生了中断。
    // 0xC004-0xC007 (PRDT Address): 存储 PRD 表的 32位物理地址，每个寄存器是 8 位的，因此有 4 个寄存器
    uint32_t bmba = bar4 & 0xFFFC; 
    if (bmba == 0) return -1;

    // 激活硬件 Bus Master 模式 (整个设备只需执行一次)
    uint32_t cmd = pci_read_config(dev->bus, dev->slot, dev->func, PCI_CMD_REG_OFFSET);
    // 设置 PCI_COMMAND_MASTER，让 pci 设备可以窃取 cpu 的总线周期
    // 这个 config read 加 write 的本质就是改了一下 cmd 寄存器的 bus master 位
    // 这两个操作是对 pci 接口的操作，而不是对 pci 设备的操作
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_CMD_REG_OFFSET, cmd | PCI_COMMAND_MASTER);

    // 为每个通道配置资源
    for (int i = 0; i < CHANNEL_NUM; i++) {
        struct ide_channel* chan = &channels[i];
        
        // 分配并清零 PRD 表内存
        void* page = get_kernel_pages(1); 
        if (!page) {
            printk("IDE Error: Failed to allocate PRD table memory.\n");
            continue; 
        }
        memset(page, 0, PG_SIZE);
        
        chan->prd_table = (struct prd*)page;
        chan->prd_table_phys = addr_v2p((uint32_t)page);
        // 每个 channel 有 8 个端口，转到下一个 channel 时我们要跳过这些端口
        chan->bmba = bmba + (i * PORT_NUM);
        
        // 告诉硬件 PRD 表的物理地址
        // outl 在硬件上会直接把一个 32 位的数据拆分写到连续的 4 个 8 位的端口上
        outl(chan->bmba + BM_PRDT_ADDR_REG_OFFSET, chan->prd_table_phys);
        
        chan->dma_enabled = true;
        printk("IDE: Channel %d (0x%x) DMA enabled. BMBA: 0x%x\n", 
                i, chan->port_base, chan->bmba);
    }

    return 0; // 返回 0 表示初始化成功
}

// 将虚拟地址缓冲区映射到 PRD 表中
// chan 渠道结构体
// buf 缓冲区虚拟地址
// size 传输字节数
// is_write 是否为写操作
static void ide_dma_setup(struct ide_channel* chan, void* buf, uint32_t size, bool is_write) {
    uint32_t vaddr = (uint32_t)buf;
    uint32_t bytes_left = size;
    int prd_idx = 0;

    while (bytes_left > 0) {
        // 计算当前物理页内剩余的可读/写长度
        uint32_t offset = vaddr & 0xFFF; // 页面内偏移
        uint32_t page_left = PG_SIZE - offset; // 物理页剩下的空间
        uint32_t chunk_size = (bytes_left < page_left) ? bytes_left : page_left;

        // 获取当前虚拟地址对应的物理地址
        uint32_t paddr = addr_v2p(vaddr);

        // 填充一个 PRD 条目
        chan->prd_table[prd_idx].paddr = paddr;
        // IDE 规范规定 0 表示 64KB，这里 chunk_size 最大只有 4KB，所以没问题
        chan->prd_table[prd_idx].size = (uint16_t)chunk_size;
        // 暂时清零 flags
        // 默认标记为 0，如果是最后一个条目则设为 0x80 (EOT)
        chan->prd_table[prd_idx].flags = 0;

        // 更新步进
        bytes_left -= chunk_size;
        vaddr += chunk_size;
        prd_idx++;

        // 防止 PRD 表溢出（通常一页能放 512 个 PRD 条目，足够用了）
        // 为了防止出现这样的情况损害系统，我们先直接 PANIC
        if (prd_idx >= 512) {
            PANIC("IDE Error: Too many PRD entries needed!\n");
            break;
        }
    }

    // 标记 PRD 表结束
    // EOT 是最后两个字节的最高位，即 0x8000
    chan->prd_table[prd_idx - 1].flags = 0x8000;

    // 设置总线主控寄存器 (Bus Master Registers)
    // 设定方向 Bit 3 (0=Write, 1=Read)
    uint8_t cmd = is_write ? 0x00 : BM_CMD_READ;
    outb(chan->bmba + BM_COMMAND_REG_OFFSE, cmd);

    // 清除状态位，IDE DMA 的设计很特殊，状态寄存器里的中断位和错误位是写 1 清零
    // 通过写 1 清除 Bit 1 (Error) 和 Bit 2 (Interrupt)
    // 这一步非常重要，否则上次传输遗留的标志位会干扰本次操作
    uint8_t status = inb(chan->bmba + BM_STATUS_REG_OFFSE);
    outb(chan->bmba + BM_STATUS_REG_OFFSE, status | BM_STATUS_INT | BM_STATUS_ERROR);
}

void ide_read_dma(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    // printk("dma read\n");
    struct ide_channel* chan = hd->my_channel;
    
    // 锁定通道，防止多个进程同时竞争同一组寄存器
    lock_acquire(&chan->lock);

    // 准备 PRDT 表、设置方向、清除状态位
    // 读操作，所以 is_write 为 false
    ide_dma_setup(chan, buf, sec_cnt * SECTOR_SIZE, false);

    // 设置 LBA 地址和扇区数，同时选择 disk
    select_sector(hd, lba, sec_cnt); 
    
    // 向磁盘控制器发送 DMA 读命令 (0xC8)
    // 此时磁盘会开始把数据从盘片读入它内部的 Buffer
    cmd_out(hd->my_channel, CMD_DMA_READ);

    // 正式开启 PCI Bus Master DMA
    // 这一步必须在发送命令之后，因为磁盘需要时间准备 DMARQ 信号
    uint8_t bm_cmd = inb(chan->bmba + BM_COMMAND_REG_OFFSE);
    // 发送 start 命令开始传输
    // 每当我们发送 start 后，DMA 控制器都会从头扫描 prdt 
    outb(chan->bmba + BM_COMMAND_REG_OFFSE, bm_cmd | BM_CMD_START);

    // 阻塞当前进程，等待中断唤醒
    // DMA 会在后台静默搬运数据，CPU 此时可以去跑其他任务
    chan->expecting_intr = true;
    sema_wait(&chan->wait_disk);

    // 中断处理程序执行完成后，进程在这里醒来
    // 检查 DMA 状态，看是否发生错误
    uint8_t status = inb(chan->bmba + BM_STATUS_REG_OFFSE);
    if (status & BM_STATUS_ERROR) {
        printk("IDE Error: DMA read failed at LBA 0x%x\n", lba);
        // 同样，如果有问题我们先 PANIC 防止系统被破坏
        // 真的出现具体的问题了我们再来处理
        PANIC("DMA Error");
    }

    // 必须关闭 DMA 引擎，将 start 命令取消就行
    outb(chan->bmba + BM_COMMAND_REG_OFFSE, inb(chan->bmba + BM_COMMAND_REG_OFFSE) & ~BM_CMD_START);

    // 释放通道锁
    lock_release(&chan->lock);
}

void ide_write_dma(struct disk* hd, uint32_t lba, void* buf, uint32_t sec_cnt) {
    // printk("dma write\n");
    struct ide_channel* chan = hd->my_channel;
    
    lock_acquire(&chan->lock);

    // 准备 PRDT 表，is_write 设为 true
    // 这里的 buf 指向的数据必须已经准备好，
    // 因为一旦下一步 BM_START 开启，DMA 控制器会立刻读取内存
    ide_dma_setup(chan, buf, sec_cnt * SECTOR_SIZE, true);

    // 告知硬盘我们要写的 LBA 地址
    select_sector(hd, lba, sec_cnt); 

    // 发送 DMA 写命令 (0xCA)
    cmd_out(chan, CMD_DMA_WRITE);

    // 开启总线主控开启传输
    uint8_t bm_cmd = inb(chan->bmba + BM_COMMAND_REG_OFFSE);
    outb(chan->bmba + BM_COMMAND_REG_OFFSE, bm_cmd | BM_CMD_START);

    // 阻塞等待
    chan->expecting_intr = true;
    sema_wait(&chan->wait_disk);

    // 检查错误
    uint8_t status = inb(chan->bmba + BM_STATUS_REG_OFFSE);
    if (status & BM_STATUS_ERROR) {
        PANIC("DMA Write Error");
    }

    // 停止 DMA 引擎
    outb(chan->bmba + BM_COMMAND_REG_OFFSE, inb(chan->bmba + BM_COMMAND_REG_OFFSE) & ~BM_CMD_START);

    lock_release(&chan->lock);
}

void ide_pci_driver_init() {
    dlist_push_back(&pci_drivers_list, &ide_pci_driver.driver_tag);
}