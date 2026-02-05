%include "boot.inc"
DISK_PARAM_ADDR equ 0x501
DISK_NUM_PTR equ 0x475
FIRST_DISK_NO equ 0x80
SECTION MBR vstart=MBR_START_ADDR

	; ebx: 0x00000700 1792
	; ecx: 0x0009783f 620607
	; edx: 0x00000f02 3842
	; cx = [1001] 01111000 00 111111 
	; dh 0f 16 heads
	; dl 02    
	; cl[5:0] 111111B=63
	; ch 0x78
	; 

	; ecx: 0x0009a13f 631103
	; edx: 0x00000f02 3842
	; ebx: 0x00000700 1792

	; cx  10100001 00 111111
	; dh 0f
	; dl 02 

	xor edi,edi ; ES:DI = 0:0（avoid BIOS error）
	mov es, di

	mov byte cl,[DISK_NUM_PTR] ; get the number of the disk
	
	mov ebx,DISK_PARAM_ADDR
	
	mov esi,FIRST_DISK_NO ; set the first disk number
get_disk_param:
	; get the disk parameter
	mov ah, 0x08      ; function number
	mov dx, si        ; disk number
	push ecx
	int 0x13
	mov [ebx],ecx
	mov [ebx+4],edx
	pop ecx
	add ebx,8
	inc esi
	loop get_disk_param

	mov ax,cs
	mov ds,ax
	mov es,ax
	mov ss,ax
	mov fs,ax
	mov bp,MBR_START_ADDR
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

	
	; push LOADER_SECTOR_CNT ; how many sector to write
	mov edi,LOADER_BASE_ADDR
	mov ebx,LOADER_START_SECTOR
	mov ecx,LOADER_SECTOR_CNT
	call rd_disk_m_16

	jmp LOADER_BASE_ADDR
;	jmp 0xc15 ; 0xc15 is loader_start


rd_disk_m_16:
    ; 设置读取扇区数
    mov dx, PRIMARY_SECTOR_CNT
    mov al, cl
    out dx, al

    ; 设置 LBA 地址，ebx 中存储的是起始扇区
    mov dx, PRIMARY_LBA_LOW
    mov al, bl
    out dx, al
    shr ebx, 8
    
    mov dx, PRIMARY_LBA_MID
    mov al, bl
    out dx, al
    shr ebx, 8

    mov dx, PRIMARY_LBA_HIGH
    mov al, bl
    out dx, al
    shr ebx, 8

    mov dx, PRIMARY_DEVICE
    mov al, 0xe0 
    or al, bl
    out dx, al

    ; 发送读取命令 0x20
    mov dx, PRIMARY_COMMAND
    mov al, 0x20
    out dx, al

    ; 开始按扇区读取数据
    ; 此时 cl 存的是要读的扇区总数 (LOADER_SECTOR_CNT)
    mov bx, di               ; 目标内存地址 0x900

.read_one_sector:
    push cx                  ; 保存外层循环次数（剩余扇区数）

    ; 读取每一个扇区前都要检查状态
.not_ready:
    mov dx, PRIMARY_COMMAND  ; 即 Status 端口 0x1f7
    in al, dx
    and al, 0x88             ; 检查 BSY 和 DRQ
    cmp al, 0x08             ; 期望 BSY=0, DRQ=1
    jnz .not_ready

    ; 读取该扇区的 256 个字 (512 字节)
    mov cx, 256              ; 内部循环 256 次，每次读 2 字节
    mov dx, PRIMARY_DATA     ; 0x1f0 端口
.go_on_read:
    in ax, dx
    mov [bx], ax
    add bx, 2
    loop .go_on_read         ; 这个循环读完 512 字节

    pop cx                   ; 恢复外层循环次数
    loop .read_one_sector    ; 如果还要读下一个扇区，跳回 .not_ready

    ret

	times 510-($-$$) db 0
	db 0x55,0xaa
	
