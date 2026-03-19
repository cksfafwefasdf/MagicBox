BUILD="./build"
BIN="prog_pipe"
LIB="../lib"
CFLAGS="-Wall -Wl,--strip-all -c -fno-builtin -W -Wstrict-prototypes -Wmissing-prototypes -m32 -Wsystem-headers -fno-stack-protector"
OBJS="../build/string.o ../build/syscall.o \
	../build/stdio.o ../build/assert.o ./build/start.o"
DD_IN=$BUILD/$BIN
DD_OUT="/opt/bochs/hd60M.img"

nasm ./start.s -f elf -o $BUILD/start.o
ar rcs $BUILD/simple_crt.a $OBJS $BUILD/start.o
gcc-4.4 $CFLAGS -I $LIB -o $BUILD/$BIN".o" $BIN".c"
ld -m elf_i386 $BUILD/$BIN".o" $BUILD/simple_crt.a -o $BUILD/$BIN
SEC_CNT=$(ls -l $BUILD/$BIN|awk '{printf("%d",($5+511)/512)}')

if [ -f "$BUILD/$BIN" ];then
	dd if=./$DD_IN of=$DD_OUT bs=512 count=$SEC_CNT seek=600 conv=notrunc
fi