; main boot record
SECTION MBR vstart=0x7c00 ; let 0x7c00 become the begining of the mbr
	mov ax,cs ; because 0x7c00 is the begining of mbr,so value of the cs is 0 (0x7c00=0x0:0x7c00)
	mov ds,ax ; we need to set all the sreg's value to 0,because the cs is 0,mbr only works on 0 segment
		  ; so addr of ds won't above 0
		  ; we can't pass a immediate number from cs to ds directly,so we need the assistance of ax
	mov es,ax
	mov ss,ax
	mov fs,ax
;init the addr for graphics card,we can also put the addr in the es and fs
	mov ax,0xb800
	mov gs,ax
; beginning of the mbr is 0x7c00,so the addr below 0x7c00 won't be occupy by the mbr
; we can use those part as stack
	mov sp,0x7c00

; INT:0x10 function_number:0x06 description:clear the window
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

; we use the 0x3 function to get the position of the cursor
        mov ah,0x3
        mov bh,0 ; bh is the page of the cursor that you want to get
        int 0x10

; print the string
; the character consist of two bytes
; the upper byte is attribute of the character,include the font-color and background-color
; the lower byte is the ascii of the character
; we can write the character to the graphics memory directly
	mov byte [gs:0x00],'1'
	mov byte [gs:0x01],0xA4 ; A means the blinking-green background-color , 4 means red font-color
	
	mov byte [gs:0x02],' '
	mov byte [gs:0x03],0xA4

	mov byte [gs:0x04],'M'
	mov byte [gs:0x05],0xA4

	mov byte [gs:0x06],'R'
	mov byte [gs:0x07],0xA4

	mov byte [gs:0x08],'R'
	mov byte [gs:0x09],0xA4


        jmp $ ; endless-loop 

        message db "1 MBR"
        times 510-($-$$) db 0 ; keyword "times" as same as "dup"
        db 0x55,0xaa

