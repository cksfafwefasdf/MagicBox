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

TAB_SPACE_SIZE equ 0x4 ; tab means 4 spaces

NUM_FULL_LINE_CH equ 80
NUM_FULL_SCREEN_LINE equ 25
NUM_FULL_SCREEN_CH equ NUM_FULL_SCREEN_LINE*NUM_FULL_LINE_CH
LAST_LINE_BEGINNING equ NUM_FULL_SCREEN_CH-NUM_FULL_LINE_CH


SELECTOR_GRAPHIC equ (0x0003<<3)+TI_GDT+RPL0

[bits 32]

section .data
	; ANSI 30-37 对应的 VGA 属性值表
    ; 索引: 0    1    2    3               4    5    6    7
    ; 颜色: 黑   红   绿   黄（高亮棕色）   蓝   紫   青   白
    color_table db COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE

	put_int_buff dq 0 ;buffer for char-int conversion
	; 存储当前字符颜色的变量，默认为 0x07 (白字)
	current_char_style db COLOR_WHITE

	; 状态机状态定义
	; 0: 普通字符, 1: 接收到 ESC, 2: 接收到 [, 3: 接收到参数
	out_status db 0

	ansi_param_val dw 0    ; 用于累加当前的参数值

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
	mov al, [out_status] ; get current status

	cmp al, 0
    jz .status_normal
    cmp al, 1
    jz .status_esc
    cmp al, 2
    jz .status_bracket
    
	jmp .put_char_done	


; status 0
.status_normal:	

	cmp cl, CH_ESC
    jz .go_to_s1

	cmp cl,CH_CR ; char type is 1byte=1bit,so cl is enough
	jz .is_carriage_return
	
	cmp cl,CH_LF
	jz .is_line_feed

	cmp cl,CH_BS
	jz .is_backspace

	cmp cl,CH_TAB
	jz .is_tab

	jmp .put_other

.go_to_s1:
    mov byte [out_status], 1
    jmp .put_char_done

; status 1, get ESC, waiting for '['
.status_esc:
    cmp cl, '['
    jz .go_to_s2
    mov byte [out_status], 0 ; illegal sequence，reset status
    jmp .put_char_done

.go_to_s2:
    mov byte [out_status], 2
    jmp .put_char_done

; status 2, get '[', waiting for arguments
.status_bracket:
    ; 当前处于 [ 之后的状态，ecx 中是当前字符
    
    ; 判断是否是结束符 'm' 
    cmp cl, 'm'
    jz .handle_m_command

	; 判断是否是结束符 'J' (屏幕处理)
    cmp cl, 'J'
    jz .handle_J_command

    ; 判断是否是分号 ';' (多参数分隔符，为以后可能的多参数扩展预留)
    cmp cl, ';'
    jz .handle_semicolon

    ; 判断是否是数字 '0'-'9'
    cmp cl, '0'
    jl .not_digit
    cmp cl, '9'
    jg .not_digit

    ; 数字累加逻辑：ansi_param_val = ansi_param_val * 10 + (cl - '0')
    push eax
    push edx
    
    movzx eax, word [ansi_param_val]
    mov edx, 10
    mul edx                ; eax = eax * 10
    
    sub cl, '0'            ; 将字符转换为数值
    movzx edx, cl
    add eax, edx           ; 加上当前个位数
    
    mov [ansi_param_val], ax ; 存回内存
    
    pop edx
    pop eax
    jmp .put_char_done     ; 解析完一位数字，等待下一个字符

.handle_J_command:
    ; ANSI 标准中，[2J 表示清空全屏
    movzx eax, word [ansi_param_val]
    cmp eax, 2
    jne .J_done             ; 如果不是 2，暂时不处理（ANSI 还有 0J, 1J）

    ; 因为此时已经在 put_char 内部，bx 寄存器存着当前光标，我们需要重置它
    pushad                  ; 保护当前 put_char 的现场
    call cls_screen         ; 调用全局清屏函数
    popad                   ; 恢复现场

    ; 清屏后，我们需要同步更新当前 put_char 里的 bx 寄存器
    ; 否则 put_char 结尾的 set_cursor 会把光标设置回老地方
    xor bx, bx              ; bx 置 0，回到左上角

.J_done:
    mov word [ansi_param_val], 0
    mov byte [out_status], 0
    jmp .set_cursor         ; 强制跳转到设置光标处，让光标归位

.handle_semicolon:
    ; 如果遇到分号，通常意味着第一个参数结束，我们可以根据需要把参数存入数组
    ; 但是目前我们只处理一个参数的情况，可以简单地清零准备读下一个，或者忽略它
    mov word [ansi_param_val], 0
    jmp .put_char_done

.not_digit:
    ; 收到既不是数字也不是 'm' 的非法字符，强制复位状态机防止卡死
    mov byte [out_status], 0
    mov word [ansi_param_val], 0
    jmp .put_char_done

.handle_m_command:
    ; 根据解析出来的数值切换颜色
    push eax
	push ebx
	; movzx(Move with Zero-Extend) 带零扩展的传送指令
    movzx eax, word [ansi_param_val]

    ;  重置颜色的情况
    cmp eax, 0
    jz .set_default

    ; 设置颜色 (30-37) 
    cmp eax, 30
    jl .m_done ; 小于 30，不支持
    cmp eax, 37
    jg .m_done ; 大于 37，不支持

    ; 计算索引: index = eax - 30
    sub eax, 30
    
    ; 查表取值
    movzx ebx, ax ; 将索引转入 ebx，高位清零
    mov al, [color_table + ebx] ; 使用 ebx 作为偏移
    mov [current_char_style], al
    jmp .m_done

.set_default:
    mov byte [current_char_style], COLOR_WHITE ; 白字
    jmp .m_done

.m_done:
	pop ebx
    pop eax
    mov word [ansi_param_val], 0 ; 处理完命令，计数器清零
    mov byte [out_status], 0      ; 状态机回到正常模式
    jmp .put_char_done


.is_tab:
	mov cx,TAB_SPACE_SIZE
.print_tab:
	shl bx,1
	mov byte [gs:bx],CH_SPACE
	inc bx

	push eax                      
    mov al, [current_char_style]  
    mov byte [gs:bx], al ; low 1byte is ASCII of char,high 1byte is style of char
    pop eax  
	
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
	
	push eax                      
    mov al, [current_char_style]  
    mov byte [gs:bx], al ; low 1byte is ASCII of char,high 1byte is style of char
    pop eax                       
	
	shr bx,1 ;equals bx/2,let offset become cursor's position
	jmp .set_cursor

.put_other:
	shl bx,1 ;bx*2, get offset from graphic segment
	
	mov byte [gs:bx],cl
	inc bx
	
	push eax                      
    mov al, [current_char_style]  
    mov byte [gs:bx], al ; low 1byte is ASCII of char,high 1byte is style of char
    pop eax  

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
	
	push eax                      
    mov al, [current_char_style]  
    mov byte [gs:bx], al ; low 1byte is ASCII of char,high 1byte is style of char
    pop eax  

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
	
	push eax                      
    mov al, [current_char_style]  
    mov byte [gs:bx], al ; low 1byte is ASCII of char,high 1byte is style of char
    pop eax  

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


global set_appearance
set_appearance:
    push ebp
    mov ebp, esp
    mov al, [ebp + 8]          ; 获取参数 color
    mov [current_char_style], al ; 更新全局颜色变量
    pop ebp
    ret