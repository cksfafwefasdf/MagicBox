BUILD_DIR = ./build
ENTRY_POINT = 0xc0001500
AS = nasm
CC = gcc-4.4
LD = ld
LIB = -I lib/ -I lib/kernel/ -I lib/user/ -I kernel/ -I device/ -I thread/ -I userprog/	-I fs/ -I shell/
ASFLAGS = -f elf
CFLAGS = -Wall $(LIB) -c -fno-builtin -W -Wstrict-prototypes \
     -Wmissing-prototypes -m32 -fno-stack-protector
LDFLAGS = -Ttext $(ENTRY_POINT) -e main -Map $(BUILD_DIR)/kernel.map -m elf_i386
# LDFLAGS = -Ttext $(ENTRY_POINT) -e main -Map $(BUILD_DIR)/kernel.map -m elf_i386 -s

OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/init.o $(BUILD_DIR)/interrupt.o \
    $(BUILD_DIR)/print.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/timer.o \
	$(BUILD_DIR)/debug.o $(BUILD_DIR)/memory.o $(BUILD_DIR)/bitmap.o \
	$(BUILD_DIR)/string.o $(BUILD_DIR)/thread.o $(BUILD_DIR)/dlist.o \
	$(BUILD_DIR)/switch.o $(BUILD_DIR)/sync.o $(BUILD_DIR)/console.o \
	$(BUILD_DIR)/keyboard.o $(BUILD_DIR)/ioqueue.o $(BUILD_DIR)/tss.o \
	$(BUILD_DIR)/process.o $(BUILD_DIR)/syscall-init.o $(BUILD_DIR)/syscall.o \
	$(BUILD_DIR)/stdio.o  $(BUILD_DIR)/stdio-kernel.o $(BUILD_DIR)/ide.o \
	$(BUILD_DIR)/fs.o $(BUILD_DIR)/dir.o $(BUILD_DIR)/inode.o $(BUILD_DIR)/file.o \
	$(BUILD_DIR)/fork.o $(BUILD_DIR)/exec.o $(BUILD_DIR)/wait_exit.o\
	$(BUILD_DIR)/shell.o $(BUILD_DIR)/buildin_cmd.o $(BUILD_DIR)/assert.o $(BUILD_DIR)/pipe.o 

$(BUILD_DIR)/main.o:kernel/main.c lib/kernel/print.h lib/stdint.h kernel/init.h
	$(CC) $< $(CFLAGS) -o $@

# C code
$(BUILD_DIR)/init.o:kernel/init.c kernel/init.h lib/kernel/print.h lib/stdint.h kernel/interrupt.h device/timer.h thread/thread.h kernel/memory.h device/console.h device/keyboard.h userprog/tss.h userprog/syscall-init.h device/ide.h fs/fs.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/interrupt.o:kernel/interrupt.c kernel/interrupt.h lib/stdint.h kernel/global.h lib/kernel/io.h lib/kernel/print.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/timer.o:device/timer.c device/timer.h lib/stdint.h lib/kernel/io.h lib/kernel/print.h kernel/debug.h thread/thread.h kernel/interrupt.h kernel/debug.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/debug.o:kernel/debug.c kernel/debug.h lib/kernel/print.h lib/stdint.h kernel/interrupt.h lib/user/syscall.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/string.o:lib/string.c lib/string.h lib/stdint.h lib/assert.h
	$(CC) $< $(CFLAGS) -o $@
	
$(BUILD_DIR)/bitmap.o:lib/kernel/bitmap.c lib/kernel/bitmap.h lib/stdint.h lib/string.h kernel/debug.h kernel/interrupt.h lib/kernel/print.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/memory.o:kernel/memory.c kernel/memory.h lib/stdint.h lib/kernel/print.h kernel/debug.h lib/string.h kernel/global.h thread/sync.h kernel/interrupt.h lib/kernel/stdio-kernel.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/dlist.o:lib/kernel/dlist.c lib/kernel/dlist.h kernel/interrupt.h  lib/stdint.h lib/stdbool.h kernel/debug.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/thread.o:thread/thread.c thread/thread.h  lib/string.h lib/stdint.h kernel/global.h kernel/memory.h kernel/interrupt.h kernel/debug.h lib/kernel/print.h lib/kernel/dlist.h userprog/process.h thread/sync.h kernel/main.h lib/stdio.h fs/fs.h fs/file.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/sync.o:thread/sync.c thread/sync.h kernel/interrupt.h kernel/debug.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/console.o:device/console.c device/console.h lib/kernel/print.h lib/stdint.h thread/sync.h thread/thread.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/keyboard.o:device/keyboard.c device/keyboard.h  lib/kernel/print.h kernel/interrupt.h lib/kernel/io.h kernel/global.h lib/stdbool.h device/ioqueue.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/ioqueue.o:device/ioqueue.c device/ioqueue.h  kernel/debug.h kernel/interrupt.h lib/stdint.h kernel/global.h lib/stdbool.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/tss.o:userprog/tss.c userprog/tss.h lib/stdint.h kernel/global.h lib/kernel/print.h lib/string.h thread/thread.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/process.o:userprog/process.c userprog/process.h thread/thread.h kernel/memory.h lib/stdint.h kernel/debug.h userprog/tss.h device/console.h lib/string.h kernel/global.h kernel/interrupt.h lib/kernel/dlist.h lib/kernel/print.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/syscall-init.o:userprog/syscall-init.c userprog/syscall-init.h lib/stdint.h thread/thread.h lib/kernel/print.h lib/user/syscall.h device/console.h lib/string.h fs/fs.h userprog/fork.h thread/thread.h userprog/exec.h userprog/wait_exit.h device/ide.h shell/pipe.h
	$(CC) $< $(CFLAGS) -o $@
 
$(BUILD_DIR)/syscall.o:lib/user/syscall.c lib/user/syscall.h lib/stdint.h thread/thread.h lib/kernel/print.h device/console.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/stdio.o:lib/stdio.c lib/stdio.h kernel/global.h lib/stdint.h lib/string.h lib/user/syscall.h fs/file.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/stdio-kernel.o:lib/kernel/stdio-kernel.c lib/kernel/stdio-kernel.h lib/stdio.h device/console.h fs/fs.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/ide.o:device/ide.c device/ide.h lib/stdint.h lib/kernel/stdio-kernel.h kernel/debug.h lib/stdio.h lib/kernel/io.h device/timer.h kernel/interrupt.h lib/string.h fs/fs.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/fs.o:fs/fs.c fs/fs.h fs/inode.h fs/super_block.h fs/dir.h kernel/debug.h lib/string.h device/ide.h lib/kernel/stdio-kernel.h lib/stdbool.h lib/stdint.h lib/kernel/dlist.h fs/file.h device/console.h thread/thread.h device/ioqueue.h device/keyboard.h shell/pipe.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/inode.o:fs/inode.c fs/inode.h fs/fs.h fs/super_block.h fs/file.h device/ide.h lib/stdint.h kernel/debug.h lib/kernel/dlist.h lib/string.h thread/thread.h kernel/interrupt.h lib/kernel/stdio-kernel.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/file.o:fs/file.c fs/file.h fs/fs.h fs/inode.h  fs/super_block.h fs/dir.h kernel/debug.h lib/string.h device/ide.h lib/kernel/stdio-kernel.h lib/stdbool.h lib/kernel/dlist.h lib/stdint.h lib/string.h thread/thread.h kernel/debug.h kernel/interrupt.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/dir.o:fs/dir.c fs/dir.h fs/inode.h fs/file.h device/ide.h fs/super_block.h lib/kernel/stdio-kernel.h lib/string.h lib/stdint.h lib/stdbool.h kernel/debug.h 
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/fork.o:userprog/fork.c userprog/fork.h lib/stdint.h thread/thread.h lib/string.h userprog/process.h kernel/debug.h fs/file.h kernel/interrupt.h lib/kernel/dlist.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/shell.o:shell/shell.c shell/shell.h lib/stdio.h fs/fs.h kernel/debug.h lib/user/syscall.h fs/file.h lib/string.h lib/stdbool.h lib/assert.h shell/buildin_cmd.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/buildin_cmd.o:shell/buildin_cmd.c shell/buildin_cmd.h kernel/debug.h fs/file.h fs/fs.h lib/string.h lib/user/syscall.h shell/buildin_cmd.h lib/stdio.h fs/dir.h lib/assert.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/exec.o:userprog/exec.c userprog/exec.h lib/stdint.h lib/stdbool.h kernel/global.h fs/fs.h kernel/memory.h lib/string.h thread/thread.h lib/stdio.h lib/user/syscall.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/assert.o:lib/assert.c lib/assert.h lib/stdio.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/wait_exit.o:userprog/wait_exit.c userprog/wait_exit.h thread/thread.h lib/stdint.h lib/stdbool.h fs/fs.h lib/kernel/dlist.h fs/file.h kernel/debug.h
	$(CC) $< $(CFLAGS) -o $@

$(BUILD_DIR)/pipe.o:shell/pipe.c shell/pipe.h lib/stdbool.h lib/stdint.h fs/fs.h fs/file.h device/ioqueue.h thread/thread.h lib/stdio.h lib/kernel/stdio-kernel.h
	$(CC) $< $(CFLAGS) -o $@

# ASM code

$(BUILD_DIR)/kernel.o:kernel/kernel.s
	$(AS) $< $(ASFLAGS) -o $@

$(BUILD_DIR)/print.o:lib/kernel/print.s
	$(AS) $< $(ASFLAGS) -o $@

$(BUILD_DIR)/switch.o:thread/switch.s
	$(AS) $< $(ASFLAGS) -o $@

# link file
$(BUILD_DIR)/kernel.bin:$(OBJS)
	$(LD) $^ $(LDFLAGS) -o $@

.PHONY:mk_dir hd clean all boot gdb_symbol

boot:$(BUILD_DIR)/mbr.bin $(BUILD_DIR)/loader.bin
$(BUILD_DIR)/mbr.bin:boot/mbr.s
	$(AS) -I boot/inc/ -o $(BUILD_DIR)/mbr.bin boot/mbr.s
	
$(BUILD_DIR)/loader.bin:boot/loader.s
	$(AS) -I boot/inc/ -o $(BUILD_DIR)/loader.bin boot/loader.s

mk_dir:
	if [ ! -d $(BUILD_DIR) ]; then mkdir $(BUILD_DIR); fi

hd:
	dd if=$(BUILD_DIR)/mbr.bin of=/opt/bochs/hd60M.img count=1 bs=512 conv=notrunc
	dd if=$(BUILD_DIR)/loader.bin of=/opt/bochs/hd60M.img count=4 bs=512 conv=notrunc seek=2
	dd if=$(BUILD_DIR)/kernel.bin of=/opt/bochs/hd60M.img bs=512 count=200 seek=9 conv=notrunc 

clean: 
	cd $(BUILD_DIR) && rm -f *.o *.bin

build:$(BUILD_DIR)/kernel.bin

gdb_symbol:
	objcopy --only-keep-debug $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/kernel.sym

all:mk_dir boot build hd gdb_symbol