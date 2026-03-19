TI_GDT equ 0
RPL0 equ 0
CH_CR equ 0xd ; carriage return
CH_LF equ 0xa ; line feed
CH_BS equ 0x8 ; backspace
CH_TAB equ 0x9 ; tab
CH_SPACE equ 0x20
CH_STYLE equ 0x07
CH_STR_END equ 0x0

TAB_SPACE_SIZE equ 0x4 ; tab means 4 spaces

NUM_FULL_LINE_CH equ 80
NUM_FULL_SCREEN_LINE equ 25
NUM_FULL_SCREEN_CH equ NUM_FULL_SCREEN_LINE*NUM_FULL_LINE_CH
LAST_LINE_BEGINNING equ NUM_FULL_SCREEN_CH-NUM_FULL_LINE_CH


SELECTOR_GRAPHIC equ (0x0003<<3)+TI_GDT+RPL0

[bits 32]

section .data

put_int_buff dq 0 ;buffer for char-int conversion

section .text

global put_char
put_char: ; write a char that from the stack to the position of cursor 
	pushad ; backup all the GPRs
	
	mov ax,SELECTOR_GRAPHIC
	mov gs,ax
	
	; get cursor's position
	mov dx,0x3d4 ;3d4h is CRT Controller addr reg
	mov al,0x0e ;0x0e is cusor-location-high reg
	out dx,al
	mov dx,0x3d5 ; data reg
	in al,dx
	mov ah,al

	mov dx,0x3d4 ; still CRT Controller Addr reg
	mov al,0x0f ; 0x0f is cursor-location-low reg
	out dx,al ; index cursor-location-low reg
	mov dx,0x3d5
	in al,dx

	mov bx,ax ;bx has cursor-location
	; pushad push 8 GPRs which is 4*8=32byte
	; after pushad's 32byte is return-addr which is 4byte
	; so first argument is esp+32+4=esp+36
	mov ecx,[esp+36] ; get char to print 
	
	cmp cl,CH_CR ; char type is 1byte=1bit,so cl is enough
	jz .is_carriage_return
	
	cmp cl,CH_LF
	jz .is_line_feed

	cmp cl,CH_BS
	jz .is_backspace

	cmp cl,CH_TAB
	jz .is_tab

	jmp .put_other


.is_tab:
	mov cx,TAB_SPACE_SIZE
.print_tab:
	shl bx,1
	mov byte [gs:bx],CH_SPACE
	inc bx
	mov byte [gs:bx],CH_STYLE
	shr bx,1
	inc bx
	loop .print_tab
	jmp .set_cursor

.is_backspace:
	; cursor move one character backward	
	; then replace the charater with space-character

	; if bx equals 0, it means the cursor on the beginning of the screen
	; we can not move backward ! 
	cmp bx,0
	jz .put_char_done
	
	dec bx
	shl bx,1 ; equals bx*2,get offset
	
	mov byte [gs:bx],CH_SPACE ;0x20 is space-character
	inc bx
	mov byte [gs:bx],CH_STYLE ; low 1byte is ASCII of char,high 1byte is style of char
	shr bx,1 ;equals bx/2,let offset become cursor's position
	jmp .set_cursor

.put_other:
	shl bx,1 ;bx*2, get offset from graphic segment
	
	mov byte [gs:bx],cl
	inc bx
	mov byte [gs:bx],CH_STYLE
	shr bx,1
	inc bx
	cmp bx,NUM_FULL_SCREEN_CH ; if screen is full,then start a new line
	jl .set_cursor

; \n and \r behave the same way which is CRLF,CR+LF
.is_line_feed: ; LF (\n)
.is_carriage_return: ; CR (\r)  
	xor dx,dx ;dx is high 16bit of dividend
	mov ax,bx ;ax is low 16bit of dividend
	mov si,NUM_FULL_LINE_CH

	div si ;[dx:ax]/[si]->dx:ax (dx is remainder), get cursor's line num

	; cursor's position subtract the remainder, bx round down,let the cursor move to the beginning of the line
	sub bx,dx

.is_carriage_return_end: ; CR process end
	; if is LF, let cursor's position + 80
	add bx,NUM_FULL_LINE_CH ; move to next line,now cursor in the beginning of the next line 
	cmp bx,NUM_FULL_SCREEN_CH ;check whether need to roll screen

.is_line_feed_end: ; LF process end
	jl .set_cursor


.roll_screen:
	cld ;set DF=0,si,di extend upward
	; we need carry 2000-80=1920 chars,which is 1920*2=3840bytes in total
	; we can carry 4byte each times,so we need carry 3840/4=960times
	;mov ecx,960
	mov ecx,960
	; beginning of the graphic-segment is 0xc00b8000
	mov esi,0xc00b80a0 ; head of the first line (80ch * 2byte/ch =160byte=0xa0)
	mov edi,0xc00b8000 ; head of the zero line
	rep movsd ; move data byte-by-byte

	; fill the last line with space-character
	mov ebx,3840 ; offset of the last line,24*80*2
	mov ecx,NUM_FULL_LINE_CH ; each line has 80 chars

.cls:
	mov byte [gs:ebx],CH_SPACE
	inc ebx
	mov byte [gs:ebx],CH_STYLE
	inc ebx
	loop .cls
	mov bx,LAST_LINE_BEGINNING ; cursor move to the beginning of the last line

.set_cursor:
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

.put_char_done:
	popad
	ret


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

cls_loop:
	mov byte [gs:ebx],CH_SPACE
	inc ebx
	mov byte [gs:ebx],CH_STYLE
	inc ebx
	loop cls_loop
	mov ebx,0 ; cursor move to the beginning of the screen

cls_screen_set_cursor:
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