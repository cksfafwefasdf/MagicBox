%include "boot.inc"
section loader vstart=LOADER_BASE_ADDR
LOADER_STACK_TOP equ LOADER_BASE_ADDR
	jmp loader_start
GDT_BASE: 
	dd 0x00000000
	dd 0x00000000

CODE_DESC:
	dd 0x0000FFFF
	dd DESC_CODE_HIGH32

DATA_STACK_DESC:
	dd 0x0000FFFF
	dd DESC_DATA_HIGH32

GRAPHIC_DESC:
	dd 0x80000007
	dd DESC_GRAPHIC_HIGH32

GDT_SIZE equ $-GDT_BASE
GDT_LIMIT equ GDT_SIZE-1
times 60 dq 0

; gdt is 512 Byte 
; 512 Byte = 0x200 Byte
; jmp loader_start is 3 Byte
; 0x900+0x200+0x3=0xb03
; total_mem_byte in 0xb03
total_mem_bytes dd 0

SELECTOR_CODE equ (0x0001<<3)+TI_GDT+RPL0
SELECTOR_DATA equ (0x0002<<3)+TI_GDT+RPL0
SELECTOR_GRAPHIC equ (0x0003<<3)+TI_GDT+RPL0
; 0xb07
gdt_ptr dw GDT_LIMIT
		dd GDT_BASE

; total_mem_byte(4B)+gdt_ptr(6B)+ards_buf(244B)+ards_nr(2B) = 256 Byte
; 0xb0d
ards_buf times 244 db 0
ards_num dw 0

loadermsg db '2 loader in real.'

error_hlt:
	hlt ; if an error occurs, suspend CPU

loader_start:
	xor ebx,ebx
	mov edx,0x534d4150
	mov di,ards_buf
e820_mem_get_loop:
	mov eax,0x0000e820 ; after int 0x15,eax will be 0534d4150,so resend the function number
	mov ecx,20 ;each ards has 20bytes
	int 0x15
	jc e820_failed_try_e801 ; check CF
	add di,cx
	inc word [ards_num]
	cmp ebx,0
	jnz e820_mem_get_loop
	
	mov cx,[ards_num]
	mov ebx,ards_buf
	xor edx,edx ; clear edx
get_avl_mem:
	mov eax,[ebx+16] ;get type
	and eax,11b
	cmp eax,0x1 ; check if OS could use
	jnz os_can_not_use
	add edx,[ebx+8]
os_can_not_use:
	add ebx,20 ;mov to next ards
	loop get_avl_mem
	jmp mem_get_ok

e820_failed_try_e801:
	mov ax,0xe801
	int 0x15
	jc e801_failed_try_88

; get low 0MB~15MB
	mov cx,0x400 ; granularity is 1KB
	mul cx ; [ax]*[cx]->dx:ax
	shl edx,16 ; leave low 16bit for eax
	and eax,0x0000FFFF ;clear high 16bit
	or edx,eax ; integrate ax and dx
	add edx,0x100000 ; add 1MB
	mov esi,edx ; low 16MB put into esi
; get high 16MB~4GB
	xor eax,eax
	mov ax,bx
	mov ecx,0x10000 ;granularity is 64KB
	mul ecx ; [eax]*[ecx]->edx:eax
; the upper bound e801 can get is 4GB
; so 32bit-eax is enough, edx will be 0x00000000
	add esi,eax
	mov edx,esi
	jmp mem_get_ok

e801_failed_try_88:
	mov ah,0x88
	int 0x15
	jc error_hlt
	and eax,0x0000FFFF
	mov cx,0x400 ; granularity is 1KB
	mul cx ; [ax]*[cx]->dx:ax
; integrate dx and ax
	shl edx,16
	or edx,eax
	add edx,0x100000 ; add 1MB

mem_get_ok:
	mov [total_mem_bytes],edx ;store the result

;------- print message ----------
	;mov sp,LOADER_BASE_ADDR
	;mov bp,loadermsg
	;mov cx,17	
	;mov ax,0x1301
	;mov bx,0x001f
	;mov dx,0x1800
	;int 0x10
;------- open A20Gate ----------
	in al,0x92
	or al,0000_0010B
	out 0x92,al
;------- load GDT ----------
	lgdt [gdt_ptr]
;------- set cr0 ----------
	mov eax,cr0
	or eax,0x00000001
	mov cr0,eax

; refresh the pipeline,flush the pipeline and update the selector in es
	jmp SELECTOR_CODE:p_mode_start

[bits 32]
p_mode_start:
	mov ax,SELECTOR_DATA
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov esp,LOADER_STACK_TOP
    mov ax,SELECTOR_GRAPHIC
    mov gs,ax
	mov byte [gs:160],'P'
;------- load kernel ----------
	push KERNEL_START_SECTOR
	push KERNEL_BIN_BASE_ADDR
	push KERNEL_SECTOR_CNT
	; rd_disk_m_32(KERNEL_START_SECTOR,KERNEL_BIN_BASE_ADDR,200)
	; 0x000000000d1c
	call rd_disk_m_32 ;read kernel from disk
	pop eax
	pop eax
	pop eax
	xor eax,eax

;------- init pagetable and turn on PG ----------
	call setup_page

	sgdt [gdt_ptr] ;backup gdt
	
	mov ebx,[gdt_ptr+2] ;gdt_ptr+2 is gdt_base
	or dword [ebx+0x18+4],0xc0000000 ;get gdt-item's high 32bit then 'or' 
	
	add dword [gdt_ptr+2],0xc0000000 ; gdt_base =  gdt_base + 3GB 
	
	add esp,0xc0000000 

	mov eax,PAGE_DIR_TABLE_POS
	mov cr3,eax

	mov eax,cr0
	or eax,0x80000000 ; let PG true
	mov cr0,eax

	; use v_addr reload gdt
	lgdt[gdt_ptr]

	mov byte [gs:160],'V'
	; 0xd71
	jmp SELECTOR_CODE:enter_kernel


setup_page:
	mov ecx,4096
	mov esi,0
clear_page_dir: ; clear 4KB in the beginning of 0x100000
	mov byte [PAGE_DIR_TABLE_POS+esi],0
	inc esi
	loop clear_page_dir	

create_pde: ; create page_dir_table item
	mov eax,PAGE_DIR_TABLE_POS
	add eax,0x1000 ; 4KB/PAGE, add 4KB on the base of PAGE_DIR_TABLE to get the beginning of the first PAGE_TABLE
	mov ebx,eax
	or eax,PG_US_U|PG_RW_W|PG_P
	
	mov [PAGE_DIR_TABLE_POS+0x0],eax
	mov [PAGE_DIR_TABLE_POS+0xc00],eax
	
	sub eax,0x1000
	mov [PAGE_DIR_TABLE_POS+4092],eax

	mov ecx,256 ; we only init 1MB
	mov esi,0
	xor edx,edx
	mov edx,PG_US_U|PG_RW_W|PG_P

create_pte:
	mov [ebx+esi*4],edx
	add edx,4096
	inc esi
	loop create_pte

	mov eax,PAGE_DIR_TABLE_POS 
	add eax,0x2000
	or eax,PG_US_U|PG_RW_W|PG_P
	mov ebx,PAGE_DIR_TABLE_POS
	mov ecx,254
	mov esi,769

create_kernel_pde:
	mov [ebx+esi*4],eax
	inc esi
	add eax,0x1000
	loop create_kernel_pde
	ret

; rd_disk_m_32(KERNEL_START_SECTOR,KERNEL_BIN_ADDR,200)
; [esp+4] get 200
rd_disk_m_32:
	; set sector num
	mov dx,PRIMARY_SECTOR_CNT
	mov eax,[esp+4] ;get sector cnt
	out dx,al
	
	mov eax,[esp+12] ;get START_SECTOR
	;set LBA_LOW 0~7
	mov dx,PRIMARY_LBA_LOW
	out dx,al
	mov cl,8
	shr eax,cl
	;set LBA_MID 8~15
	mov dx,PRIMARY_LBA_MID
	out dx,al
	shr eax,cl
	;set LBA_HIGH 16~23
	mov dx,PRIMARY_LBA_HIGH
	out dx,al
	shr eax,cl
	;set LBA_DEVICE 24~27,and set DEVICE
	and al,0x0f
	or al,1110_0000b
	;or al,0xe0
	mov dx,PRIMARY_DEVICE
	out dx,al

	;set command
	mov al,0x20
	mov dx,PRIMARY_COMMAND
	out dx,al
	
	mov dx,PRIMARY_STATUS
not_ready: 
	nop
	; first operand must be al !!! can not be ax !!!
	; beacuse status-port is 8bit ,use ax instead of al may cause hardware fault!!!
	in al,dx
	and al,0x88
	cmp al,0x08
	jnz not_ready

	; sector is ready to transmit
	mov eax,256; data-port is 16bit,a block is 512Byte 512*8/16=256 times
	mov ecx,[esp+4] ; get sector cnt
	mul ecx ; 256times/sector * 200sector
	mov ecx,eax ; result put into ecx
	mov edx,PRIMARY_DATA
	mov ebx,[esp+8] ;get kernel.bin base addr
keep_read:
	in ax,dx
	mov [ebx],ax
	add ebx,2
	loop keep_read	
	ret

enter_kernel:
	call kernel_init
	; 0xe58
	mov esp,0xc009f000
	mov byte [gs:162],' '
	mov byte [gs:164],'K'
	jmp KERNEL_ENTRY_POINT
	
kernel_init: ; put code_segment into mem
	xor eax,eax
	xor ebx,ebx ;ebx->addr of program header table
	xor ecx,ecx ;ecx->program header num
	xor edx,edx ;edx->phentsize
	
	mov dx,[KERNEL_BIN_BASE_ADDR+42] ;get phentsize,phentsize is Elf32_Half which is 2Byte,so use dx instead of edx
	mov cx,[KERNEL_BIN_BASE_ADDR+44] ;get ph number
	mov ebx,[KERNEL_BIN_BASE_ADDR+28] ;get offset of pht
	add ebx,KERNEL_BIN_BASE_ADDR ;get addr of pht

each_segment:
	cmp byte [ebx+0],PT_NULL ; ebx+0 get p_type which is 32bit,but the type only range from 0 to 6,so byte is enough
	je PTNULL

	; if not NULL_SEGMENT then put it into mem
	; memcpy(dest,src,size) push auguments from right to left !!!
	push dword [ebx+16] ; p_filesz
	mov eax,[ebx+4] ; p_offset
	add eax,KERNEL_BIN_BASE_ADDR
	push eax
	push dword [ebx+8] ;p_vaddr is dest
	call mem_cpy
	pop eax
	pop eax
	pop eax
	xor eax,eax
PTNULL:
	add ebx,edx ;move to next segment
	loop each_segment

	ret ;return to kernel_init instead of return from PTNULL !

; mem_cpy(dest,src,size)
mem_cpy:
	cld ; clean direction,let DF=0,di and si extend upward
	
	push ebp ; backup ebp
	mov ebp,esp ;change stack frame
	
	push ecx; rep will use ecx,ecx has ph_num,so backup ecx

	;ebp+4 is return addr
	mov edi,[ebp+8] ; dest
	mov esi,[ebp+12] ; src
	mov ecx,[ebp+16] ; size,granularity is byte
	rep movsb ; copy byte by byte
	; 0xeb7
	pop ecx
	pop ebp
	ret

