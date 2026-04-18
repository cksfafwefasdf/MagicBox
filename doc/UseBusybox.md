# 在系统中使用 Busybox

## 安装过程

首先下载 [busybox 1.23.2](https://busybox.net/downloads/busybox-1.23.2.tar.bz2) ，随后解压源码，然后进入源码根目录，执行下面的命令

```shell
# 清理编译环境
make distclean
```

之后运行下面的指令取消 busybox 中所有的子工具，稍后我们在一一按需打开。

```shell
# 关闭所有工具，再依次打开
make allnoconfig
```

进入 GUI 配置，然后打开我们需要的选项

```shell
# 进行配置
make ARCH=i386 menuconfig
```

关闭下面的选项

```shell
Busybox Settings -> General Configuration -> Enable Linux-specific applets and features 
```

打开下面的选项

```shell
Busybox Settings -> General Configuration -> Don't use /usr 
Busybox Settings -> Build Options ->  Build BusyBox as a static binary (no shared libs)
Busybox Settings -> Busybox Library Tuning -> Command line editing # 只有开启此参数才能启用行编辑

Coreutils -> cat
Coreutils -> touch
Coreutils -> cp
Coreutils -> dd
Coreutils -> echo
Coreutils -> ln
Coreutils -> ls
Coreutils -> mkdir
Coreutils -> mv
Coreutils -> pwd
Coreutils -> rm
Coreutils -> rmdir
Coreutils -> seq
Coreutils -> stat

Editors -> vi

Finding Utilities -> grep

Shells -> ash (需要取消其下的  bash-compatible extensions 子项)
```

修改 `include/libbb.h` 文件的 BUG_off_t_size_is_misdetected 字段，将

```c
struct BUG_off_t_size_is_misdetected {
        char BUG_off_t_size_is_misdetected[sizeof(off_t) == sizeof(uoff_t) ? 1 : -1];
};
```

改为

```c
struct BUG_off_t_size_is_misdetected {
        char BUG_off_t_size_is_misdetected[1];
};
```

之后进行编译

```shell
make clean

make ARCH=i386 \
  CC="musl-gcc -m32 -Wl,-m,elf_i386" \
  HOSTCC="gcc" \
  CFLAGS="-m32 -march=i486 -fno-stack-protector" \
  LDFLAGS="-m32 -static -Wl,-m,elf_i386" \
  -j$(nproc)
```

此时会产生 busybox_unstripped 和 busybox 两个文件

执行下面的命令后，会将 busybox 安装到相应的 `$HOME/busybox-install` 目录并创建符号链接

```shell
make ARCH=i386 \
  CC="musl-gcc -m32 -Wl,-m,elf_i386" \
  HOSTCC="gcc" \
  CFLAGS="-m32 -march=i486 -fno-stack-protector" \
  LDFLAGS="-m32 -static -Wl,-m,elf_i386" \
  CONFIG_PREFIX=$HOME/busybox-install \
  install
```

随后将 `/bin` 目录整个拷贝到 `hd60M.img` 的根目录下即可。



## 使用编译好的文件

如果上面的步骤出现问题，可以使用实现编译好的软件包，进入项目根目录的 `third_party` 目录下，之后解压压缩包

```shell
tar -zvxf busybox_env.tar.gz
```

然后和安装 tcc 的步骤一样，直接将解压出的 `bin` 目录下的所有文件都拷贝到 `hd60M.img` 的根目录下就行

进入项目根目录下的 `tool` 目录，运行

```shell
sh bind.sh
```

这会将 `hd60M.img` 的第一个分区 `sda1` 转换为循环设备后挂载到宿主机的 `disk_env/mnt` 目录下，之后进入这个 `mnt` 目录，执行

```
sudo mv ../../third_party/bin ./
```

之后回到 `tool` 目录，运行

```shell
# 需要注意的是，unbind 会删除你机器上所有的循环设备，先确定你是否有不该被删除的循环设备！
# 如果有则不要运行这个脚本！而是去手动卸载刚刚 bind 挂载的设备！
sh unbind.sh
```

现在进入系统，不出意外的话应该使用的 shell 就是 ash 了，而不是我们自己的 mbsh

进入系统以后，可能会看到一个提示 `in path /etc/passwd, file passwd not exist`，这个无伤大雅，因为我们的目录结构不完整，所以出现这个提示是正常的，如果嫌烦可以执行下面的命令创建相应的文件。

```shell
mkdir /etc
echo root:x:0:0:root:/root:/bin/sh > /etc/passwd
```



## 注意事项

~~目前的 busybox 移植还在进行中，像是 vi 这样的大型程序的运行还有问题，还有待进一步的对接~~ （现在 vi 已经可以正常使用了）。

此外，虽然我们的系统底层已经实现了很多系统调用，但是在使用 busybox 程序的过程中可能还是会出现未知的系统调用，这有待进一步对接。