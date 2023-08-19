;----------macro defination file----------

;MBR will be loaded into this position
MBR_START_ADDR equ 0x7c00



;this is port for the text mode of graphics card
;the addr is actually 0xb8000
;because the sreg will shr the addr automaticly,so we need make the addr shl first
GRAPHICS_TEXT_START equ 0xb800



;the port in the disk
DISK_PRIMARY_DATA equ 0x1f0
DISK_PRIMARY_ERROR_FEATURES equ 0x1f1
DISK_PRIMARY_SECTOR_COUNT equ 0x1f2
DISK_PRIMARY_LBA_LOW equ 0x1f3
DISK_PRIMARY_LBA_MID equ 0x1f4
DISK_PRIMARY_LBA_HIGH equ 0x1f5
DISK_PRIMARY_DEVICE equ 0x1f6
DISK_PRIMARY_STATUS_COMMAND equ  0x1f7
DISK_PRIMARY_CONTROL equ 0x3f6

DISK_SECONDARY_DATA equ 0x170
DISK_SECONDARY_ERROR_FEATURES equ 0x171
DISK_SECONDARY_SECTOR_COUNT equ 0x172
DISK_SECONDARY_LBA_LOW equ 0x173
DISK_SECONDARY_LBA_MID equ 0x174
DISK_SECONDARY_LBA_HIGH equ 0x175
DISK_SECONDARY_DEVICE equ 0x176
DISK_SECONDARY_STATUS_COMMAND equ 0x177
DISK_SECONDARY_CONTROL equ 0x376




;---------- loader and kernel ----------

;0x500~x7bff and 0x7e00~9fbff this two areas in the memory are free,so we can put the loader into 0x900
LOADER_BASE_ADDR equ 0x900

;the 0x1 block in the disk is used to store the mbr,so we use the 0x2 block to store the loader,we use the LBA to access the block in the disk
LOADER_START_SECTOR equ 0x2



;---------- gdt descriptor attribute ----------
; the underline '_' is meaningless,it will be removed while compiling
; the high 32 bits: base_addr(31~24) G(23) D/B(22) L(21) AVL(20) bound_addr(19~16) P(15) DPL(14~13) S(12) TYPE(11~8) base_addr(7~0)
; the low 32 bits: base_addr(31~16) bound_addr(15~0)
; because the 31~24 bits of the high 32 bits is base_addr,so the attribute bits only have 24 bits 
DESC_G_4K equ 1_00000000000000000000000b ; the granularity of memory is 4k
DESC_D_32 equ 1_0000000000000000000000b ; we set the operend and offset in the eip as 32 bits,if this bit is 0, they will be 16 bits
DESC_L equ 0_000000000000000000000b ; we set bit of the code segment as 32 bits
DESC_AVL equ 0_00000000000000000000b ; user cannot access this block
DESC_LIMIT_CODE2 equ 1111_0000000000000000b ; the bound address of the segment
DESC_LIMIT_DATA2 equ DESC_LIMIT_CODE2
DESC_LIMIT_VIDEO2 equ 0000_000000000000000b ; graphics memory is not above 512KB,so set the high 4 bits as 0 is enough
DESC_P equ 1_000000000000000b ; P means present,to indicate the segment is exist in memory or not,this bit is used to swap in/out
DESC_DPL_0 equ 00_0000000000000b ;description of privilege level
DESC_DPL_1 equ 01_0000000000000b 
DESC_DPL_2 equ 10_0000000000000b 
DESC_DPL_3 equ 11_0000000000000b 
DESC_S_CODE equ 1_000000000000b ;data segment and code segment are all the none-system segment
DESC_S_DATA equ DESC_S_CODE 
DESC_S_SYS equ 0_000000000000b ;system segment label
DESC_TYPE_CODE equ 1000_00000000b ;x=1,c=0,r=0,a=0 accessed clear to 0,code segment is executable,unreadable,not consistency
DESC_TYPE_DATA equ 0010_00000000b ;x=0,e=0,w=1,a=0 accessed clear to 0,data segment is unexecutable,upward-extend,writable
; the high 4 bytes of the segment
; 0x00<<24 makes 31~24 bits become 00000000b, we use the flat model just like Linux,so the base_addr are all 0
DESC_CODE_HIGH4 equ (0x00<<24)+DESC_G_4K+DESC_D_32+DESC_L+DESC_AVL+DESC_LIMIT_CODE2+DESC_P+DESC_DPL_0+DESC_S_CODE+DESC_TYPE_CODE+0x00
DESC_DATA_HIGH4 equ (0x00<<24)+DESC_G_4K+DESC_D_32+DESC_L+DESC_AVL+DESC_LIMIT_DATA2+DESC_P+DESC_DPL_0+DESC_S_DATA+DESC_TYPE_DATA+0x00
DESC_VIDEO_HIGH4 equ (0x00<<24)+DESC_G_4K+DESC_D_32+DESC_L+DESC_AVL+DESC_LIMIT_VIDEO2+DESC_P+DESC_DPL_0+DESC_S_DATA+DESC_TYPE_DATA+0x0b ; graphics momory is beginning at 0xb8000,so the "b" is set in here

; video is for memory of grapihcs card

;---------- the attribute of selector ----------
;request privilege level
RPL0 equ 00b
RPL1 equ 01b
RPL2 equ 10b
RPL3 equ 11b
; use GDT or LDT
TI_GDT equ 000b 
TI_LDT equ 100b

