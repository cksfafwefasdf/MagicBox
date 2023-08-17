; main boot record
SECTION MBR vstart=0x7c00 ; let 0x7c00 become the begining of the mbr
	mov ax,cs ; because 0x7c00 is the begining of mbr,so value of the cs is 0 (0x7c00=0x0:0x7c00)
	mov ds,ax ; we need to set all the sreg's value to 0,because the cs is 0,mbr only works on 0 segment
		  ; so addr of ds won't above 0
		  ; we can't pass a immediate number from cs to ds directly,so we need the assistance of ax
	mov es,ax
	mov ss,ax
	mov fs,ax
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
; we can use the 0x13 function to print a string to the screen
        mov ax,message
        mov bp,ax ; es:bp is the begining addr of the string

        mov cx,5 ;the length of the string
        mov ax,0x1301 ; ah is the function_number,al is the write mode
        mov bx,0x2 ; bh is the page number of the string that your want to show,we use the 0 page
        ;bl is the text style 02h means black background and green font-color
        int 0x10

        jmp $ ; endless-loop 

        message db "1 MBR"
        times 510-($-$$) db 0 ; keyword "times" as same as "dup"
        db 0x55,0xaa

