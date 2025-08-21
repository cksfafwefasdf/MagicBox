# if [[ ! -d "../lib/" || ! -d "../build/" ]];then
# 	echo "dependent dir don't exist!"
# 	cwd=$(pwd)
# 	cwd=${cwd##*/}
# 	cwd=${cwd%/}
# 	if [[ $cwd != "command" ]];then
# 		echo -e "you'd better in command dir\n"
# 	fi
# 	exit
# fi
# ENTRY_POINT=0x8049000
BUILD="./build"
BIN="prog_no_arg"
LIB="../lib"
CFLAGS="-Wall -Wl,--strip-all -c -fno-builtin -W -Wstrict-prototypes -Wmissing-prototypes -m32 -Wsystem-headers -fno-stack-protector"
OBJS="../build/string.o ../build/syscall.o \
	../build/stdio.o ../build/assert.o"
DD_IN=$BUILD/$BIN
DD_OUT="/opt/bochs/hd60M.img"

gcc-4.4 $CFLAGS -I $LIB -o $BUILD/$BIN".o" $BIN".c"
# ld -m elf_i386 -Ttext $ENTRY_POINT -e main $BUILD/$BIN".o" $OBJS -o $BUILD/$BIN
ld -m elf_i386 -e main $BUILD/$BIN".o" $OBJS -o $BUILD/$BIN
SEC_CNT=$(ls -l $BUILD/$BIN|awk '{printf("%d",($5+511)/512)}')

if [ -f "$BUILD/$BIN" ];then
	dd if=./$DD_IN of=$DD_OUT bs=512 count=$SEC_CNT seek=300 conv=notrunc
fi