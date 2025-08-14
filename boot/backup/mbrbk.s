%include "../inc/boot.inc"
SECTION MBR vstart=MBR_START_ADDR
	mov ax,cs
	mov ds,ax
	mov es,ax
	mov ss,ax
	mov fs,ax
	mov sp,MBR_START_ADDR
	mov ax,GRAPHIC_TEXT_START
	mov gs,ax
			; clear screen
	mov ax,0x0600
	mov bx,0x0700
	mov cx,0
	mov dx,0x184f
	int 0x10
			; print string
	mov byte [gs:0x00],'1'
	mov byte [gs:0x01],0xA4
	mov byte [gs:0x02],' '
	mov byte [gs:0x03],0xA4
	mov byte [gs:0x04],'M'
	mov byte [gs:0x05],0xA4
	mov byte [gs:0x06],'B'
	mov byte [gs:0x07],0xA4
	mov byte [gs:0x08],'R'
	mov byte [gs:0x09],0xA4

	push LOADER_BASE_ADDR
	push 1 ; how many sector to write
	mov ebx,LOADER_START_SECTOR
	call rd_disk_m_16
	jmp LOADER_BASE_ADDR


rd_disk_m_16:
	mov dx,PRIMARY_SECTOR_CNT ;how many sector to write
	pop cx
	mov al,cl
	out dx,al

	mov dx,PRIMARY_LBA_LOW ;write LBA low 0-7
	mov al,bl
	out dx,al
	shr ebx,8
	
	mov dx,PRIMARY_LBA_MID ;write LBA mid 8-15
	mov al,bl
	out dx,al
	shr ebx,8

	mov dx,PRIMARY_LBA_HIGH ;write LBA high 16-23
	mov al,bl
	out dx,al
	shr ebx,8

	mov dx,PRIMARY_DEVICE ;write device-port,high 4bit is 1110B
	mov ax,0xe0 
	or al,bl
	out dx,al

	mov dx,PRIMARY_COMMAND ; write commmand 'read' to check the status of disk
	mov ax,0x20
	out dx,al

not_ready:
	nop
	in al,dx
	and al,0x88
	cmp al,0x08
	jnz not_ready


		    ; read data
	mov ax,cx ; cx is 1
	mov dx,256 ; read 256 times per sector,each operate could read 2byte
	mul dx
	mov cx,ax
	mov dx,PRIMARY_DATA
	pop bx
go_on_read:
	in ax,dx
	mov [bx],ax
	add bx,2
	loop go_on_read
	ret

	times 510-($-$$) db 0
	db 0x55,0xaa
	
	





