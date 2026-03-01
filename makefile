# 路径配置 
BUILD_ROOT     := ./build
BUILD_DIR      := $(BUILD_ROOT)/kernel
BUILD_DIR_PROG := $(BUILD_ROOT)/prog
DISK_DIR       := ./disk_env
ENTRY_POINT    := 0xc0001500

# 编译器定义
AS      := nasm
CC      := gcc
LD      := ld
OBJCOPY := objcopy
OBJDUMP := objdump

# 自动化目录管理
# 定义参与内核编译的子目录（白名单）
# 这里不包含用户程序目录 prog/，因为那里的 main 函数会与内核 main 冲突
KERNEL_SUBDIRS := kernel device thread userprog fs lib mm lib/kernel lib/user lib/common

# 自动获取所有 C 和 ASM 源文件
# 使用 wildcard 查找
SRCS_C := $(foreach dir, $(KERNEL_SUBDIRS), $(wildcard $(dir)/*.c))
SRCS_S := $(foreach dir, $(KERNEL_SUBDIRS), $(wildcard $(dir)/*.s))

# 排除掉不需要的特定冲突文件
SRCS_C := $(filter-out lib/user/syscall_convey_by_stack.c, $(SRCS_C))

# 生成对应的 .o 对象列表
# 强制 main.o 排在 OBJS 的第一位
# 这样在执行 ld 链接时，main 函数的代码块会被物理地放置在二进制文件的最前面
# 确保内核的main函数位于 0xc0001500 处，以便于 loader 可以正确跳转
MAIN_OBJ := $(BUILD_DIR)/main.o
OTHER_OBJS := $(patsubst %.c, $(BUILD_DIR)/%.o, $(notdir $(filter-out kernel/main.c, $(SRCS_C))))
OTHER_OBJS += $(patsubst %.s, $(BUILD_DIR)/%.o, $(notdir $(SRCS_S)))

OBJS := $(MAIN_OBJ) $(OTHER_OBJS)

empty :=
space := $(empty) $(empty)
VPATH := $(subst $(space),:,$(KERNEL_SUBDIRS))

# 编译选项 
LIB := -I lib/ -I lib/kernel/ -I lib/user/ -I lib/common/ \
       -I kernel/ -I device/ -I thread/ -I userprog/ -I fs/ -I shell/ -I mm/

ASFLAGS := -f elf

# -MMD 会为每个 .c 文件自动生成 .d 依赖文件，记录头文件依赖
CFLAGS  := -Wall $(LIB) -g -c -fno-builtin -W -Wstrict-prototypes -D DEBUG \
           -Wmissing-prototypes -m32 -fno-stack-protector -fcommon \
           -Wno-error=implicit-function-declaration -MMD

LDFLAGS := -Ttext $(ENTRY_POINT) -e main -Map $(BUILD_DIR)/kernel.map -m elf_i386

.PHONY: all clean mk_dir hd boot build gdb_symbol disasm

all: mk_dir boot build hd gdb_symbol disasm

# 链接内核
$(BUILD_DIR)/kernel.bin: $(OBJS)
	@echo "Linking Kernel..."
	$(LD) $(LDFLAGS) $^ -o $@

# 自动 C 编译模板
$(BUILD_DIR)/%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $< -o $@

# 自动 ASM 编译模板
$(BUILD_DIR)/%.o: %.s
	@echo "Assembling $<..."
	$(AS) $(ASFLAGS) $< -o $@

# 引导程序编译 (MBR 和 Loader)
boot: $(BUILD_DIR)/mbr.bin $(BUILD_DIR)/loader.bin

$(BUILD_DIR)/mbr.bin: boot/mbr.s
	$(AS) -I boot/inc/ -f bin $< -o $@

$(BUILD_DIR)/loader.bin: boot/loader.s
	$(AS) -I boot/inc/ -f bin $< -o $@

# 引入自动生成的依赖文件
# 如果 .h 文件改了，Make 会自动重编包含它的 .c
-include $(OBJS:.o=.d)

mk_dir:
	@mkdir -p $(BUILD_DIR) $(BUILD_DIR_PROG)

hd:
	@echo "Writing to Disk Image..."
	dd if=$(BUILD_DIR)/mbr.bin of=$(DISK_DIR)/hd60M.img count=1 bs=446 conv=notrunc
	printf '\125\252' | dd of=$(DISK_DIR)/hd60M.img bs=1 count=2 seek=510 conv=notrunc
	dd if=$(BUILD_DIR)/loader.bin of=$(DISK_DIR)/hd60M.img count=4 bs=512 conv=notrunc seek=2
	dd if=$(BUILD_DIR)/kernel.bin of=$(DISK_DIR)/hd60M.img bs=512 count=700 seek=9 conv=notrunc 

clean:
	rm -rf $(BUILD_ROOT)/*

build: $(BUILD_DIR)/kernel.bin

gdb_symbol:
	$(OBJCOPY) --only-keep-debug $(BUILD_DIR)/kernel.bin $(BUILD_DIR)/kernel.sym

disasm:
	$(OBJDUMP) -b binary -m i8086 -D $(BUILD_DIR)/mbr.bin > $(BUILD_DIR)/disasm_mbr
	$(OBJDUMP) -b binary -m i386 -D $(BUILD_DIR)/loader.bin > $(BUILD_DIR)/disasm_loader