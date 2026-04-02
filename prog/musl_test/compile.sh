# 将安装目录 bin 目录下的 musl-gcc 程序使用 ln -s 创建一共符号链接后
# 放到 /usr/bin 中，就可以不给定路径直接运行了
BUILD_DIR="../../build/musl_test"

mkdir $BUILD_DIR
musl-gcc -g -O0 -fno-stack-protector -m32 -static $1 -o $BUILD_DIR/musl -Wl,-m,elf_i386
