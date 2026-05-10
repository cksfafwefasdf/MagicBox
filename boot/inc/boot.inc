;------- gdt defination ----------

DESC_G_4K equ 1_00000000000000000000000b       
DESC_D_32 equ 1_0000000000000000000000b        
DESC_L_32 equ 0_000000000000000000000b	    
DESC_AVL equ 0_00000000000000000000b	    
DESC_LIMIT_CODE2  equ 1111_0000000000000000b        
DESC_LIMIT_DATA2  equ DESC_LIMIT_CODE2              
DESC_LIMIT_GRAPHIC2  equ 0000_000000000000000b        
DESC_P equ	1_000000000000000b            
DESC_DPL_0 equ 00_0000000000000b            
DESC_DPL_1 equ 01_0000000000000b            
DESC_DPL_2 equ 10_0000000000000b            
DESC_DPL_3 equ 11_0000000000000b            
DESC_S_CODE equ	1_000000000000b            
DESC_S_DATA equ	DESC_S_CODE                       
DESC_S_SYS equ 0_000000000000b            
DESC_TYPE_CODE equ 1000_00000000b	        ;x=1,c=0,r=0,a=0 
DESC_TYPE_DATA equ 0010_00000000b	        ;x=0,e=0,w=1,a=0

DESC_CODE_HIGH32 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 + DESC_L_32 + DESC_AVL + DESC_LIMIT_CODE2 + DESC_P + DESC_DPL_0 + DESC_S_CODE + DESC_TYPE_CODE + 0x00
DESC_DATA_HIGH32 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 + DESC_L_32 + DESC_AVL + DESC_LIMIT_DATA2 + DESC_P + DESC_DPL_0 + DESC_S_DATA + DESC_TYPE_DATA + 0x00
DESC_GRAPHIC_HIGH32 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 + DESC_L_32 + DESC_AVL + DESC_LIMIT_GRAPHIC2 + DESC_P + DESC_DPL_0 + DESC_S_DATA + DESC_TYPE_DATA + 0x0b

RPL0 equ 00b
RPL1 equ 01b                                   
RPL2 equ 10b                                   
RPL3 equ 11b                                    
TI_GDT equ 000b                                 
TI_LDT equ 100b                                 

;------- memory defination ----------

MBR_START_ADDR equ 0x7c00
GRAPHIC_TEXT_START equ 0xb800

;------- disk defination ----------

PRIMARY_DATA equ 0x1f0
PRIMARY_ERROR equ 0x1f1
PRIMARY_FEATURES equ 0x1f1
PRIMARY_SECTOR_CNT equ 0x1f2
PRIMARY_LBA_LOW equ 0x1f3
PRIMARY_LBA_MID equ 0x1f4
PRIMARY_LBA_HIGH equ 0x1f5
PRIMARY_DEVICE equ 0x1f6
PRIMARY_STATUS equ 0x1f7
PRIMARY_COMMAND equ 0x1f7
PRIMARY_ALTERNATE_STATUS equ 0x3f6
PRIMARY_DEVICE_CONTROL equ 0x3f6

SECONDARY_DATA equ 0x170
SECONDARY_ERROR equ 0x171
SECONDARY_FEATURES equ 0x171
SECONDARY_SECTOR_CNT equ 0x172
SECONDARY_LBA_LOW equ 0x173
SECONDARY_LBA_MID equ 0x174
SECONDARY_LBA_HIGH equ 0x175
SECONDARY_DEVICE equ 0x176
SECONDARY_STATUS equ 0x177
SECONDARY_COMMAND equ 0x177
SECONDARY_ALTERNATE_STATUS equ 0x376
SECONDARY_DEVICE_CONTROL equ 0x376

; ------- loader and kernel----------

; 物理地址范围       长度,          用途    
; 0x000-0x3FF       1KB,        中断向量表 (IVT)
; 0x500-0x7BFF      ~30KB       自由空间
; 0x7C00-0x7DFF     512B        MBR 代码
; 0x7E00-0x9FBFF    ~600KB      自由空间
; 0xA0000-0xBFFFF   128KB       显存/视频缓冲 (VGA)
; 0xC0000-0xFFFFF   256KB       BIOS ROM / 映射区
; 内核入口的 0xc0001500 会被映射到物理内存的 0x1500 处
; 使用 objdump -p ./build/kernel/kernel.bin 可以查看内核在实际开始运行时，会占用多大的内存
; 主要关注 memsz 字段，filesz 字段是其在磁盘上的大小而不是内存中的大小
; 在引入 DMA 后，查看镜像大小，memsz 总共为 0x515FC 大概是 330KB，但是实际我们写入磁盘的大小是 226KB = 0x37198
; 这里有 100 多 KB 的增大是由 BSS 等数据段展开后导致的
; KERNEL_BIN_BASE_ADDR 在 0x60000
; 并且 0x60000 + 0x37198 = 0x97198，不会和 VGA 缓冲区踩踏
; 0x1500+0x515FC=0x52AFC 刚好不会触及到 0x60000
; 但是把内核入口放在 0x1500 会把 MBR 给盖了，但是我们不用了，所以没关系
; 但是如果内核继续增大的话就一定要注意！必须时刻注意是否会发生踩踏
; 由于我们的内核时 elf 格式的，因此我们不能一开始就直接把他拷贝到 0x1500
; 我们需要先解析符号表，之后再拷贝，所以这个搬运操作是必不可少的
LOADER_BASE_ADDR equ 0x900
LOADER_START_SECTOR equ 0x2
LOADER_SECTOR_CNT equ 0x4
PAGE_DIR_TABLE_POS equ 0x100000
KERNEL_START_SECTOR equ 0x9
KERNEL_BIN_BASE_ADDR equ 0x60000
KERNEL_SECTOR_CNT equ 0x384 ; 0x384=900
KERNEL_ENTRY_POINT equ 0xc0001500

;------- page table defination ----------

PG_P equ 1b
pG_RW_R equ 00b
PG_RW_W equ 10b
PG_US_S equ 000b
PG_US_U equ 100b

;------- segment type ----------

PT_NULL equ 0
PT_LOAD equ 1
PT_DYNAMIC equ 2
PT_INTERP equ 3
PT_NOTE equ 4
PT_SHLIB equ 5
PT_PHDR equ 6
PT_LOPROC equ 0x70000000
PT_HIPROC equ 0x7fffffff
