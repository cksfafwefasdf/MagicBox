TI_GDT equ 0
RPL0 equ 0
CH_CR equ 0xd ; carriage return
CH_LF equ 0xa ; line feed
CH_BS equ 0x8 ; backspace
CH_TAB equ 0x9 ; tab
CH_SPACE equ 0x20
CH_STR_END equ 0x0
CH_ESC equ 0x1b

COLOR_BLACK equ 0x00
COLOR_BLUE equ 0x0B
COLOR_GREEN equ 0x0A
COLOR_CYAN equ 0x03
COLOR_RED equ 0x0C
COLOR_MAGENTA equ 0x05
COLOR_YELLOW equ 0x0e
COLOR_WHITE equ 0x0f ; 默认的输出风格为白色

TAB_SPACE_SIZE equ 0x8 ; tab means 8 spaces

SCREEN_WIDTH equ 80
SCREEN_HEIGHT equ 25
NUM_FULL_SCREEN_CH equ SCREEN_HEIGHT*SCREEN_WIDTH
LAST_LINE_BEGINNING equ NUM_FULL_SCREEN_CH-SCREEN_WIDTH


SELECTOR_GRAPHIC equ (0x0003<<3)+TI_GDT+RPL0

[bits 32]

section .data
	put_int_buff dq 0 ;buffer for char-int conversion
	; 存储当前字符颜色的变量，默认为 0x07 (白字)
	global current_char_style
	current_char_style db COLOR_WHITE

section .text
extern put_char

global put_str
put_str:
	push ebx ;backup ebx and ecx
	push ecx
	xor ecx,ecx
	;get str addr (ebx and ecx is 8byte,return-addr is 4byte)
	;so str-addr is esp+8+4=esp+12
	mov ebx,[esp+12]
.goon:
	mov cl,[ebx]
	cmp cl,CH_STR_END
	jz .str_over
	push ecx ; convey argument to put_char
	call put_char
	add esp,4 ; recycle the argument-stack-mem
	inc ebx ; move to next byte
	jmp .goon
.str_over:
	pop ecx
	pop ebx
	ret

global put_int
; put number as 16-based
put_int:
	pushad
	; pushad has already backup ebp and esp
	mov ebp,esp ;switch stack frame
	mov eax,[ebp+36] ; still GPRs+ret-addr = 36,get the int
	mov edx,eax
	; init-offset in put_int_buff(we store data as large-segment-character-order)
	mov edi,7
	mov ecx,8 ;32bit binary num equals 8 16-based nums
	mov ebx,put_int_buff

; process the number as 16based
.16based_process:
	and edx,0x0000000f ;get low 4bits to convert it into 16based num
	cmp edx,9 	
	jg .is_A2F
	add edx,'0'
	jmp .store

.is_A2F:
	sub edx,10 ; 0xa~0xf sub 10 get the offset 
	add edx,'A' ; offset add 'A' to get the ASCII to print

; store the character as large-character-order
.store:
	mov [ebx+edi],dl
	dec edi
	shr eax,4
	mov edx,eax
	loop .16based_process

.ready_to_print:
	mov edi,0
.skip_prefix_zero:
	;input number is a 8bit 16based number,so we only process
	;8 bits
	cmp edi,8
	
	je .full0 ;if it has 8 zeros in the prefix,then the num must be 0
.go_on_skip:
	mov cl,[put_int_buff+edi]
	inc edi
	cmp cl,'0'
	je .skip_prefix_zero
	dec edi
	jmp .put_each_num

.full0:
	mov cl,'0'
.put_each_num:
	push ecx 
	call put_char
	add esp,4
	inc edi
	mov cl,[put_int_buff+edi]
	cmp edi,8
	jl .put_each_num
	popad
	ret

; set_cursor(uint32_t pos)
global set_cursor
set_cursor:

	push ebp
	mov ebp,esp
	pushad
	
	; pos
	; ret_addr
	; ebp <-ebp
	; ...
	mov ecx,[ebp+8] ;get pos
	; set low 8 bits
	mov dx,0x3d4
	mov al,0x0f
	out dx,al
	mov dx,0x3d5
	mov al,cl
	out dx,al
	; set high 8 bits
	mov dx,0x3d4
	mov al,0x0e
	out dx,al
	
	mov dx,0x3d5
	mov al,ch
	out dx,al

	popad
	pop ebp
	ret

global cls_screen
cls_screen: 
	pushad 
	mov ax,SELECTOR_GRAPHIC 
	mov gs,ax

	mov ebx,0
	mov ecx,NUM_FULL_SCREEN_CH

	; 提前把要写入的属性准备好，以免在循环里不停赋值
    mov ah, [current_char_style]  ; 属性字节
    mov al, CH_SPACE     ; 字符字节 (0x20)

.cls_loop:
	; 利用 ax 一次性写入两个字节（字符+属性）
    ; 这样就不需要写两次显存，也不需要频繁 inc ebx
    mov [gs:ebx], ax    ; 往 [gs:ebx] 写入 al, [gs:ebx+1] 写入 ah
    add ebx, 2          ; 每次移动两个字节
    loop .cls_loop

    ; 逻辑光标归零
    mov ebx, 0

.cls_screen_set_cursor:
	; set high 8 bit
	mov dx,0x03d4 ;CRT Controller addr reg
	mov al,0x0e ;cursor-location-high
	out dx,al ;index the port
	mov dx,0x03d5 ;CRT Controller data reg
	mov al,bh
	out dx,al
	;set low 8 bit
	mov dx,0x3d4
	mov al,0x0f ;cursor-location-low
	out dx,al
	mov dx,0x3d5
	mov al,bl
	out dx,al

	popad
	ret