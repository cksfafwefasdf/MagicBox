# 如何在系统中使用tcc来编译程序

## 1.安装 musl

首先，先下载 [musl-1.0.5](https://musl.libc.org/releases/musl-1.0.5.tar.gz) 的源码，然后解压后进入源码根目录

进入musl源码目录，执行下面的代码进行配置

```shell
# 彻底清理环境（包括配置文件）
make distclean

# musl 的包装器是路径敏感的，他不会自动展开 ~
# 所以最好用 $HOME
./configure \
    --target=i686 \
    --prefix=$HOME/musl-install \
    CC='gcc -m32' \
    CFLAGS='-m32 -march=i486 -fno-stack-protector' \
    --disable-shared \
    --enable-gcc-wrapper
```

之后执行下面的命令进行编译和安装

```shell
mkdir ~/musl-install
# 编译
make -j$(nproc)
# 虚拟安装到一个临时打包目录，安装在前面指定的prefix目录下
make install
```

这样，musl 库就被安装在了 prefix 所指定的目录下。

之后进行下面的操作

```shell
# 切换到安装目录
cd ~/musl-install
# （可选）为了便于操作，为 musl-gcc 包装器创建一个符号链接
ln -s $(pwd)/bin/musl-gcc musl-gcc
# （可选）将符号链接拷贝到 /usr/bin 目录下，以便在宿主机上可以直接调用
sudo mv ./musl-gcc /usr/bin
```

然后编写下面的 `test.c` 程序，使用 musl 的 gcc 包装器来编译

```c
// test.c

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>

int main() {
    printf("--- [1] Testing PID & Basic I/O ---\n");
    printf("Current PID: %d\n", getpid()); // 触发 SYS_getpid (20)

    // --- 文件操作部分 ---
    printf("\n--- [2] Testing File System (Open/Write/Close) ---\n");
    const char* filename = "/test_musl.txt";
    
    // 触发 SYS_open (5)
    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open failed");
    } else {
        printf("Open '%s' success, fd: %d\n", filename, fd);

        // 触发 SYS_write (4) 或 SYS_writev (146)
        const char* msg = "Hello from MagicBox OS via Musl-gcc!\n";
        ssize_t bytes = write(fd, msg, strlen(msg));
        printf("Written %d bytes to fd %d\n", bytes, fd);

        // 触发 SYS_lseek (19)
        off_t offset = lseek(fd, 0, SEEK_SET);
        printf("Lseek back to %lld\n", offset);

        // 触发 SYS_read (3)
        char read_buf[64] = {0};
        read(fd, read_buf, sizeof(read_buf));
        printf("Read back: %s", read_buf);

        // 触发 SYS_fstat64 (197)
        struct stat st;
        if (fstat(fd, &st) == 0) {
            printf("Fstat: Size = %lld, Inode = %llu\n", st.st_size, st.st_ino);
        }

        // 触发 SYS_close (6)
        close(fd);
        printf("File closed.\n");
    }

    // --- 内存管理部分 ---
    printf("\n--- [3] Testing Memory (Malloc/Free/Brk) ---\n");
    // 触发 SYS_brk (45) 或 SYS_mmap2 (192)
    void* ptr = malloc(1024 * 4); 
    if (ptr) {
        strcpy(ptr, "Dynamic memory is working!");
        printf("Malloc address: %p, content: %s\n", ptr, (char*)ptr);
        free(ptr);
        printf("Free success.\n");
    }

    // --- 目录操作部分 ---
    printf("\n--- [4] Testing Directory (Opendir/Getdents) ---\n");
    // 触发 SYS_open (5) 并带 O_DIRECTORY 标志，接着触发 SYS_getdents64 (220)
    DIR* dir = opendir("/");
    if (dir) {
        struct dirent* de;
        printf("Contents of '/':\n");
        while ((de = readdir(dir)) != NULL) {
            printf("  - %s (type: %d)\n", de->d_name, de->d_type);
        }
        closedir(dir);
    } else {
        perror("opendir / failed");
    }

    printf("\n--- [5] Testing Final Exit ---\n");
    // 触发 SYS_exit_group (252)
    return 0;
}
```

运行下面的指令来编译

```shell
bin/musl-gcc -g -O0 -fno-stack-protector -m32 -static test.c -o test -Wl,-m,elf_i386
```

得到 `test` 文件，编译完毕后，在宿主机上运行一下看看有没有问题，没有问题就把编译好的可运行文件拷贝到 MagicBox 上运行一下，看看有没有问题。

在 MagicBox 中的运行结果大概是这样的话，那么就没问题了，关键看 `Contents of '/':` 这一块的内容和实际根目录下的内容是否一致。

```shell
--- [1] Testing PID & Basic I/O ---
Intrcpt: warning, use lseek as llseek
Current PID: 3

--- [2] Testing File System (Open/Write/Close) ---
Open '/test_musl.txt' success, fd: 3
Written 37 bytes to fd 3
Lseek back to 0
Read back: Hello from MagicBox OS via Musl-gcc!
Fstat: Size = 37, Inode = 43
File closed.

--- [3] Testing Memory (Malloc/Free/Brk) ---
Malloc address: 0x8051010, content: Dynamic memory is working!
Free success.

--- [4] Testing Directory (Opendir/Getdents) ---
Contents of '/':
  - . (type: 4)
  - .. (type: 4)
  - lost+found (type: 4)
  - dev (type: 4)
  - mnt (type: 4)
  - bin (type: 4)

--- [5] Testing Final Exit ---
Process 3 exiting with status 0
```



## 2.安装 tcc

首先，去下载tcc的 [这个版本]( https://repo.or.cz/tinycc.git/snapshot/98765e5ebc04ea464195fa80ea5e4bbdc70a29cc.tar.gz)（不是 `0.9.27`，是 `tinycc-98765e5`，`0.9.27` 版本的链接器有很多bug，在链接阶段会出现很多问题），之后进入新版tcc源码根目录，进行编译配置。

```shell
./configure \
    --cpu=i386 \
    --cc=musl-gcc \
    --prefix=/usr \
    --elf-entry-addr=0x8048000 \
    --with-selinux \
    --static
```

由于新版的tcc会缺少一个tccdefs_.h文件，我们需要手动创建

```shell
# 确保 tccdefs_.h 是纯字符串格式
sed 's/\\/\\\\/g;s/"/\\"/g;s/$/\\n/g' include/tccdefs.h | awk '{print "\""$0"\""}' > tccdefs_.h
```

之后，在宿主机上编译 tcc (此时位于tcc源码根目录)

```shell
gcc -m32 -static -O0 -nostdlib \
    -DONE_SOURCE=1 \
    -DCONFIG_TCC_STATIC=1 \
    -DTCC_TARGET_I386=1 \
    -DCONFIG_TCC_SELINUX=1 \
    -I . -I ./include \
    -I $HOME/musl-install/include \
    -L $HOME/musl-install/lib \
    $HOME/musl-install/lib/crt1.o \
    $HOME/musl-install/lib/crti.o \
    ./tcc.c \
    $HOME/musl-install/lib/crtn.o \
    -lc -lm -ldl -lgcc -lgcc_eh \
    -o tcc
```

之后会产生一个名为 tcc 的文件，这就是我们需要的

然后，我们安装 tcc 所需要的运行时环境

```shell
# 创建安装目录
mkdir -p ~/tcc-install 
make -j$(nproc)
# 安装运行时环境
make install DESTDIR=$HOME/tcc-install 
```

执行此操作后，在 tcc-install 目录下会产生一个 usr 目录。目录拼接的逻辑是 config 里面设置的 `prefix` 加上此处的 `DESTDIR`，即 `DESTDIR/prefix`，此处为 `$HOME/tcc-install/usr`，因此会产生一个 usr 目录。此外，`prefix` 参数还会影响到 tcc 默认的库文件搜索路径。

由于 `musl` 库封装的编译器 `musl-gcc` 在编译 `libcc1.a` 的过程中会出现一些交叉编译和链接的问题，因此我们需要手动创建 `libcc1.a` 库文件。

进入到 `tinycc-98765e5/lib` 下，手动编译 `libcc1.a`

```shell
musl-gcc -m32 -O0 -c libtcc1.c ../i386-gen.c \
    -Dstatic="" \
    -fvisibility=default \
    -DTCC_TARGET_I386 \
    -I..
# 检查符号，必须出现 00000000 T __udivmoddi4
# 也就是说必须是大写的T，而不是小写的t，t表示此函数是static的
# 具有内部链接性，其他函数无法调用，tcc为了防止和外部的c语言库冲突
# 把udivmoddi4定义为了static，但是musl没有提供这个函数，所以我们需要手动包含
nm libtcc1.o | grep udivmoddi4
# 打包文件，之后就可以拷贝 libtcc1.a 到镜像了，将其拷贝到 $HOME/tcc-install/usr/lib/tcc 下
ar rcs libtcc1.a libtcc1.o i386-gen.o
```

由于某些未知的原因，我们在包含musl的库进行编译时，链接器总是会去寻找 `__stack_chk_fail` 这个参数，即使在编译时加上了 `-fno-stack-protector ` 也没办法解决，因此我们就手动给它定义个假的，然后每次编译时都带上它。

```c
// fix.c
void __stack_chk_fail_local(void)
{

        return;
}

void __stack_chk_fail(void) {

        return;
}
```

如果不想每次编译的时候都带上这个 `fix.c`，那么就把他包含到 `libcc1.a` 中，一起打包进去（推荐这么做）

```shell
# 编译得到 fix.o
../tcc -c fix.c
# 打包到生生成的 libtcc1.a 中
ar rcs libtcc1.a fix.o
```

最后，将打包好的 `libtcc1.a` 拷贝到 `tcc-install/usr/lib/tcc` 目录下

```shell
cp ./libtcc1.a $HOME/tcc-install/usr/lib/tcc
```



## 3.创建完整的编译器运行环境

经过上面两步后，`musl-install` 和 `tcc-install` 目录里面应该都有相应的安装好的库文件了，我们将 `musl-install` 下的 `include` 中的所有文件和其下的 `lib` 目录中的所有文件都分别拷贝到  `tcc-install/usr` 中的 `include` 和 `lib` 目录下即可。

```shell
cd $HOME/musl-install
cp -r ./include/* $HOME/tcc-install/usr/include
cp -r ./lib/* $HOME/tcc-install/usr/lib
```

拷贝完毕后，把 `tcc-install` 目录下的整个 `usr` 目录（包括 usr 本身）都拷贝到 MagicBox 的根目录即可。编译好的 tcc 就放在 `usr/lib/bin` 目录下，可以把他拷贝到 MagicBox 的根目录。

之后切换到 `$HOME/tcc-install/usr/bin` 下

```shell
cd $HOME/tcc-install/usr/bin
```

在宿主机上写一个测试程序，顺手和usr目录一起拷贝到 MagicBox 中，用来测试编译器是否可用。

```c
// hello.c
#include <stdio.h>
int main(){
    printf("hello world!\n");
    return 42;
}
```

在MagicBox中，使用此命令来编译一个程序

```shell
# tcc、hello.c 按照实际情况修改
tcc -static -o hello hello.c
```

在编译的时候若出现了

```shell
Intrcpt: warning, use lseek as llseek
```

这是正常的，这是我们在拦截层加上的一个提示，表示使用 `lseek` 来替代了 `llseek` ，这在小于2GB的文件下不会出现太大问题。

在MagicBox中运行后的结果如果是这样的话，那就没问题了。

```shell
ccc@magic-box:/usr/bin$ ./hello
hello world!
Process 3 exiting with status 42
```



## 4. 使用现成的 tcc 环境

如果上面的安装步骤出现问题，可以使用现成的 tcc 环境。首先，进入项目根目录下的 `third_party` 目录，然后解压压缩包

```shell
tar -zvxf tcc_env.tar.gz
```

上面的操作结束后可以得到一个 `usr` 目录。

然后，进入项目根目录下的 `tool` 目录，运行

```shell
sh bind.sh
```

这会将 `hd60M.img` 的第一个分区 `sda1` 转换为循环设备后挂载到宿主机的 `disk_env/mnt` 目录下，之后进入这个 `mnt` 目录，执行

```shell
sudo mv ../../third_party/usr ./
```

之后回到 `tool` 目录，运行

```shell
# 需要注意的是，unbind 会删除你机器上所有的循环设备，先确定你是否有不该被删除的循环设备！
# 如果有则不要运行这个脚本！而是去手动卸载刚刚 bind 挂载的设备！
sh unbind.sh
```

现在进入系统，不出意外的话应该是可以直接使用 `/usr/bin` 下的 tcc 了。
