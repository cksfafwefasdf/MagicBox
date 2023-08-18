;----------macro defination file----------



;0x500~x7bff and 0x7e00~9fbff this two areas in the memory are free,so we can put the loader into 0x900
LOADER_BASE_ADDR equ 0x900 



;the 0x1 block in the disk is used to store the mbr,so we use the 0x2 block to store the loader,we use the LBA to access the block in the disk
LOADER_START_SECTOR equ 0x2 



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

