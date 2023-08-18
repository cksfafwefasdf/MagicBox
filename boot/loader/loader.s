%include "../include/boot.inc"
section LOADER vstart=LOADER_BASE_ADDR
	mov byte[gs:0x0A],' '
        mov byte[gs:0x0B],0xA4


	mov byte[gs:0x0C],'2'
	mov byte[gs:0x0D],0xA4

        mov byte[gs:0x0E],'_'
        mov byte[gs:0x0F],0xA4

	mov byte[gs:0x10],'L'
        mov byte[gs:0x11],0xA4
	
	mov byte[gs:0x12],'O'
       	mov byte[gs:0x13],0xA4

        mov byte[gs:0x14],'A'
        mov byte[gs:0x15],0xA4

        mov byte[gs:0x16],'D'
        mov byte[gs:0x17],0xA4
	
        mov byte[gs:0x18],'E'
        mov byte[gs:0x19],0xA4

        mov byte[gs:0x1A],'R'
        mov byte[gs:0x1B],0xA4
	jmp $

