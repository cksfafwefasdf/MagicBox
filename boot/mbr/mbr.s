; main boot record

%include "../include/boot.inc" ; include the macro defination file

SECTION MBR vstart=MBR_START_ADDR ; let 0x7c00 become the begining of the mbr
	mov ax,cs ; because 0x7c00 is the begining of mbr,so value of the cs is 0 (0x7c00=0x0:0x7c00)
	mov ds,ax ; we need to set all the sreg's value to 0,because the cs is 0,mbr only works on 0 segment
		  ; so addr of ds won't above 0
		  ; we can't pass a immediate number from cs to ds directly,so we need the assistance of ax
	mov es,ax
	mov ss,ax
	mov fs,ax
;init the addr for graphics card,we can also put the addr in the es and fs
	mov ax,GRAPHICS_TEXT_START
	mov gs,ax
; beginning of the mbr is 0x7c00,so the addr below 0x7c00 won't be occupy by the mbr
; we can use those part as stack
	mov ax,MBR_START_ADDR
	sub ax,0x2
	mov sp,ax
; INT:0x10 ---function_number:0x06 ---description:clear the window
; AH = function_number
; AL = the number of slide up (0 means all)
; BH = attribute of the line 
; (CL,CH) = top-left corner of the window (X,Y)
; (DL,DH) = down-right corner of the window (X,Y)
; no return
	mov ax,0x0600 ; slide up all the lines
	mov bx,0x0700
	mov cx,0 ; (0,0) is the top-left corner
	mov dx,0x184f  ; (25,80) the max line and colunm of the screen is 25 and 80
; the begining of the line is 0,so is the column
; so the down-right corner is (24,79)=(0x4f,0x18)
	int 0x10

; print the string
; the character consist of two bytes
; the upper byte is attribute of the character,include the font-color and background-color
; the lower byte is the ascii of the character
; we can write the character to the graphics memory directly
	mov byte [gs:0x00],'1'
	mov byte [gs:0x01],0xA4 ; A means the blinking-green background-color , 4 means red font-color
	
	mov byte [gs:0x02],'_'
	mov byte [gs:0x03],0xA4

	mov byte [gs:0x04],'M'
	mov byte [gs:0x05],0xA4

	mov byte [gs:0x06],'R'
	mov byte [gs:0x07],0xA4

	mov byte [gs:0x08],'R'
	mov byte [gs:0x09],0xA4

	mov bx,LOADER_BASE_ADDR
	mov eax,LOADER_START_SECTOR	  
	mov cx,0x4 ; because the loader will finally above 512 Bytes,so we allocate 4 blocks for loader	
	call rd_disk_m_16 ;put the loader into the memory

	jmp LOADER_BASE_ADDR ;go and execute the loader

; function description: read n sectors from disk
; params:
;	eax: start sector number base on the LBA
;	bx: addr of the mermory that you want to write
;	cx: how many sectors you want to read from the disk
rd_disk_m_16:	
; set how many sector you want to read from disk
; beacsue our disk is ata0,so we use the primary
	mov dx,DISK_PRIMARY_SECTOR_COUNT
	mov esi,eax
	mov ax,cx
	out dx,al ;this port in the disk is 8 bit
; make a backup for the number of the sector that you want to read
	
; set the LBA addr
; we have 3 LBA regs,their are all 8 bits,so we use al to transfer data
	mov eax,esi 
; get the start sector,we use LBA28 as sector number,the sectoer number may up to 28 bits,so we use eax to store it
	mov si,cx ;make backup for cx
	mov cl,0x8 ; set shr number
	
	mov dx,DISK_PRIMARY_LBA_LOW
	out dx,al
	shr eax,cl

	mov dx,DISK_PRIMARY_LBA_MID
	out dx,al
	shr eax,cl

	mov dx,DISK_PRIMARY_LBA_HIGH
	out dx,al
	shr eax,cl

; device reg: 1 MOD(LBA:1,CHS:0) 1 DEV(master:0,slave:1) LBA27 LBA26 LBA25 LBA24
; so we set device reg as 1110xxxx
; now al is 0000xxxx (xxxx is LBA number)
; 11100000 or 0000xxxx -> 1110xxxx
	or al,0xe0
	mov dx,DISK_PRIMARY_DEVICE
	out dx,al

; send command---read to the disk
	mov al,0x20 ;0x20 is read sector ,0x30 is write sector
	mov dx,DISK_PRIMARY_STATUS_COMMAND
	out dx,al

; begin to read
	not_ready:
		nop; nop could use to wait 1 clock circle
		in al,dx ; read status from status_command reg
		and al,0x88 ; we use 10001000 AND al to eliminate interference causing by other bits,we only focus on the 8 bit and the 4 bit
		cmp al,0x08 ; status 00001000 means ready to work
		jnz not_ready ; if not ready,go and wait
; di holds the number of bolcks you want to read
; a block has 512 byte,a word has 2 byte,we read data by word
; to read a block,we need to read 512/2=256 times
; so we need to read 256*di times in total
	mov ax,si ; get the backup for cx
	mov dx,256	
	mul dx ; mul src : [ax]*[src]->ax
	mov cx,ax
	mov dx,DISK_PRIMARY_DATA ; if ready to read,we read the data from data reg in the disk
	
	go_on_read:
		in ax,dx 
		mov [bx],ax
		add bx,2
		loop go_on_read
		ret	

        times 510-($-$$) db 0 ; keyword "times" as same as "dup"
        db 0x55,0xaa

