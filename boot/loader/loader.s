%include "../include/boot.inc"
section LOADER vstart=LOADER_BASE_ADDR
	LOADER_STACK_TOP equ LOADER_BASE_ADDR
	jmp loader_start ; we need skip to the data below,otherwise,cpu will execute them as instruction,it may raise exception
	
; create GDT and descriptor
; because the code execute from top to bottom,so the second dd is the high 32 bits
; we use the flat model,so the base_addr is 0
; currently,we are not distinguish data segment and code segment,so the base_addr of the data segment and code segment is same
	GDT_BASE: 
		dd 0x00000000 
		dd 0x00000000 ;address OF GDT,gdt is loaded into the beginning of the memory
	CODE_DESC: 
		dd 0x0000FFFF 
		dd DESC_CODE_HIGH4 ; code descriptor
	DATA_STACK_DESC: 
		dd 0x0000FFFF 
		dd DESC_DATA_HIGH4
	VIDEO_DESC: 
		; graphics memory addr of the text mode is beginning from 0xbffff,'b' is in the 7~0 bits of the high 32 bits,
		dd 0x80000007 ; the text mode of grapics memory is from 0xbffff-0xb8000  limit=(0xbffff-0xb8000)/4k=0x7
		dd DESC_VIDEO_HIGH4 
	GDT_SIZE equ $-GDT_BASE
	GDT_LIMIT equ GDT_SIZE-1 ; limitation count from 0
	times 60 dq 0 ;dq is define quad-word,equal to 4*16=64bits,we reserve 60 descriptor\
;selector can be put into the sreg,sreg has 16 bits,so selector has 16 bits
;the address of the selector indicates the number of selectors
;NO.0 selector is used to handle the exception
	SELECTOR_CODE equ (0x0001<<3)+TI_GDT+RPL0 ;this is the NO.1 selector
	SELECTOR_DATA equ (0x0002<<3)+TI_GDT+RPL0 ;this is the NO.2 selector
	SELECTOR_VIDEO equ (0x0003<<3)+TI_GDT+RPL0 ; the NO is corresponds to the defination above
	gdt_ptr dw GDT_LIMIT 
		dd GDT_BASE ;gdtr has 48 bits,this is a 48 bits addr
	loadermsg db '2 loader in real.'

loader_start:
	mov sp,LOADER_STACK_TOP ; ss is 0,we already init it at mbr

;----------- INT:0x10 FUNCTION:0x13 DESCRIPTION:print the string ----------
;params: AH = function_number
;	 AL = print mode
;	 BH = screen page,start from 0
;	 BL = attribute
;	 CX = length of the string
;	 (DH,DL) = (line,column)
;	 es:bp = address of string
;	 no ret
	
	mov ax,0x1301 ;ah=0x13 al=0x01
	mov bx,0x001f ;1f means blue background and pink font-color
	mov cx,17
	mov dx,0x1800 ; print the string in the 24 line(start from 0) 0 column
	mov bp,loadermsg ; es is 0,we init it at the mbr
	int 0x10

;---------- ready to enter protection mode ----------
;1. open A20(close the backrolling)
;2. load the addr of the GDT into GDTR
;3. set the PE bit in the cr0 as 1

;---------- turn on the A20 ----------
	in al,0x92
	or al,0000_0010B ; set the NO.2 bit as 1
	out 0x92,al

;---------- load the addr of GDT ----------
	lgdt [gdt_ptr] ;gdt_ptr is a pointer,it point a 48 bits address

;---------- set the PE in the cr0 as 1,PE in the NO.0 bit ----------
	mov eax,cr0
	or eax,0x1
	mov cr0,eax

	jmp dword SELECTOR_CODE:p_mode_start ;flush the pipeline
;we are now in the 16 bits mode,if we enter the 32 bits mode directly,those instructions that not execute compeletly in the pipline will raise an exception

;---------- enter the protection mode ----------
[bits 32]
p_mode_start:
	mov ax,SELECTOR_DATA
	mov ds,ax
	mov es,ax
	mov ss,ax
	mov esp,LOADER_STACK_TOP
	mov ax,SELECTOR_VIDEO
	mov gs,ax

	mov byte [gs:160],'P'

	jmp $
