#include "keyboard.h"
#include "print.h"
#include "interrupt.h"
#include "io.h"
#include "global.h"
#include "stdbool.h"
#include "tty.h"

#define KBD_BUF_PORT 0x60 // keyboard buff port

#define esc '\033'
#define backspace '\b'
#define tab '\t'
#define enter '\r'
#define delete '\177'

#define char_invisible 0
#define ctrl_l_char char_invisible
#define ctrl_r_char char_invisible
#define shift_l_char char_invisible
#define shift_r_char char_invisible
#define alt_l_char char_invisible
#define alt_r_char char_invisible
#define caps_lock_char char_invisible

#define shift_l_make 0x2a
#define shift_r_make 0x36
#define alt_l_make 0x38
#define alt_r_make 0xe038
#define alt_r_break 0xe038
#define ctrl_l_make 0x1d
#define ctrl_r_make 0xe01d
#define ctrl_r_break 0xe09d
#define caps_lock_make 0x3a

// use to check if the key is pressed
// use [ext_scancode] to check if makecode is begin with 0xe0
static bool ctrl_status,shift_status,alt_status,capslock_status,ext_scancode;

// use makecode to index the array
char keymap[][2] = {
/* 0x00 */	{0,	0},		
/* 0x01 */	{esc,	esc},		
/* 0x02 */	{'1',	'!'},		
/* 0x03 */	{'2',	'@'},		
/* 0x04 */	{'3',	'#'},		
/* 0x05 */	{'4',	'$'},		
/* 0x06 */	{'5',	'%'},		
/* 0x07 */	{'6',	'^'},		
/* 0x08 */	{'7',	'&'},		
/* 0x09 */	{'8',	'*'},		
/* 0x0A */	{'9',	'('},		
/* 0x0B */	{'0',	')'},		
/* 0x0C */	{'-',	'_'},		
/* 0x0D */	{'=',	'+'},		
/* 0x0E */	{backspace, backspace},	
/* 0x0F */	{tab,	tab},		
/* 0x10 */	{'q',	'Q'},		
/* 0x11 */	{'w',	'W'},		
/* 0x12 */	{'e',	'E'},		
/* 0x13 */	{'r',	'R'},		
/* 0x14 */	{'t',	'T'},		
/* 0x15 */	{'y',	'Y'},		
/* 0x16 */	{'u',	'U'},		
/* 0x17 */	{'i',	'I'},		
/* 0x18 */	{'o',	'O'},		
/* 0x19 */	{'p',	'P'},		
/* 0x1A */	{'[',	'{'},		
/* 0x1B */	{']',	'}'},		
/* 0x1C */	{enter,  enter},
/* 0x1D */	{ctrl_l_char, ctrl_l_char},
/* 0x1E */	{'a',	'A'},		
/* 0x1F */	{'s',	'S'},		
/* 0x20 */	{'d',	'D'},		
/* 0x21 */	{'f',	'F'},		
/* 0x22 */	{'g',	'G'},		
/* 0x23 */	{'h',	'H'},		
/* 0x24 */	{'j',	'J'},		
/* 0x25 */	{'k',	'K'},		
/* 0x26 */	{'l',	'L'},		
/* 0x27 */	{';',	':'},		
/* 0x28 */	{'\'',	'"'},		
/* 0x29 */	{'`',	'~'},		
/* 0x2A */	{shift_l_char, shift_l_char},	
/* 0x2B */	{'\\',	'|'},		
/* 0x2C */	{'z',	'Z'},		
/* 0x2D */	{'x',	'X'},		
/* 0x2E */	{'c',	'C'},		
/* 0x2F */	{'v',	'V'},		
/* 0x30 */	{'b',	'B'},		
/* 0x31 */	{'n',	'N'},		
/* 0x32 */	{'m',	'M'},		
/* 0x33 */	{',',	'<'},		
/* 0x34 */	{'.',	'>'},		
/* 0x35 */	{'/',	'?'},
/* 0x36	*/	{shift_r_char, shift_r_char},	
/* 0x37 */	{'*',	'*'},    	
/* 0x38 */	{alt_l_char, alt_l_char},
/* 0x39 */	{' ',	' '},		
/* 0x3A */	{caps_lock_char, caps_lock_char}
};

static void intr_handler_keyboard(void){
	bool ctrl_down_last = ctrl_status;
	bool shift_down_last = shift_status;
	bool caps_lock_last = capslock_status;
	
	// CPU read output from 8042
	uint16_t scancode = inb(KBD_BUF_PORT);
	if(scancode==0xe0){
		ext_scancode = true;
		// we don't handle the ext_code
		return;
	}

	if(ext_scancode){
		scancode = ((0xe000)|scancode);
		ext_scancode = false;
	}

	bool is_break_code = ((scancode&0x0080)!=0); // check if it is break_code
	if(is_break_code){

		uint16_t make_code = (scancode&=0xff7f);
		if(make_code==ctrl_l_make||make_code==ctrl_r_make){
			ctrl_status = false;
		}else if(make_code==shift_l_make||make_code==shift_r_make){
			shift_status = false;
		}else if(make_code==alt_l_make||make_code==alt_r_make){
			alt_status = false;
		}
		
		return;

	}else if((scancode>0x00&&scancode<0x3b)||(scancode==alt_r_make||scancode==ctrl_r_make)){
		// if it is makecode
		// we only handle alt_right, ctrl and the character defined by keymap 


		// check whether combine with shift
		bool is_upper_case = false;

		if((scancode<0x0e)||(scancode==0x29)||(scancode==0x1a)
		||(scancode==0x1b)||(scancode==0x2b)||(scancode==0x27)
		||(scancode==0x28)||(scancode==0x33)||(scancode==0x34)
		||(scancode==0x35)){
			if(shift_down_last){
				is_upper_case = true;
			}
		}else{
			if(shift_down_last&&caps_lock_last){
				is_upper_case=false;
			}else if(shift_down_last||caps_lock_last){
				is_upper_case = true;
			}else{
				is_upper_case = false;
			}
		}
			

		uint8_t index = (scancode &= 0x00ff);
		char cur_char = keymap[index][is_upper_case];

		if((ctrl_down_last&&cur_char=='l')||(ctrl_down_last&&cur_char=='u')){
			cur_char = cur_char - 'a' + 1;
		}

		// check if cur_char is visible
		if(cur_char){
			tty_input_handler(cur_char);
			return;
		}

		if(scancode==ctrl_l_make||scancode==ctrl_r_make){
			ctrl_status = true;
		}else if(scancode == shift_l_make||scancode==shift_r_make){
			shift_status = true;
		}else if(scancode==alt_l_make||scancode==alt_r_make){
			alt_status = true;
		}else if(scancode==caps_lock_make){
			capslock_status = !capslock_status;
		}

	}else{
		put_str("unknown key!\n");
	}

	return;
}

void keyboard_init(void){
	put_str("keyboard_init start \n");
	register_handler(0x21,intr_handler_keyboard);
	put_str("keyboard_init done \n");
}