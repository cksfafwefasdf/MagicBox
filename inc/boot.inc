;macro defination file

;0x500~x7bff and 0x7e00~9fbff this two areas in the memory are free,so we can put the loader into 0x900
LOADER_BASE_ADDR equ 0x900 

;the 0x1 block in the disk is used to store the mbr,so we use the 0x2 block to store the loader,we use the LBA to access the block in the disk
LOADER_START_SECTOR equ 0x2 
