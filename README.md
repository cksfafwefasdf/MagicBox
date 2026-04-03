# MagicBox

**MagicBox** is an early-stage 32-bit x86 operating system prototype. This project serves as a personal practice work to explore the fundamental principles of operating systems, based on classic textbooks and community implementations. It replicates the core pipeline from initial bootloading to a basic user-mode environment.

> **Note**: This project is intended for learning purposes only, utilizing the traditional 32-bit Protected Mode tech stack.



## 📚 Learning Objectives

- **Bootstrapping & Environment**: Implemented a minimal MBR and Loader to handle the transition from Real Mode to 32-bit Protected Mode, along with an initial implementation of basic paging.
- **Minimalist Process Model**: Explored Unix-style process lifecycle management by replicating core system calls such as `fork()` and `execve()`.
- **Experimental App Deployment**: To simplify the deployment process, a simple **Tar-based** archiving mechanism was implemented. Binaries are written to raw disk sectors and extracted by the kernel into the file system during boot.
- **Basic Interaction**: Developed a rudimentary command-line Shell that supports internal commands and simple path resolution for external programs (e.g., searching in `/bin`).



## 🛠 Technical Overview

- **Interrupts & Synchronization**: Utilizes the **8259A PIC** to handle hardware interrupts. Kernel-level thread synchronization is managed through **mutexes and semaphores**, ensuring safe access to critical sections such as disk I/O operations and process lists.

- **Memory Management: Direct-Map Kernel + VMA-Based User Space**

  This project implements a layered memory-management design that separates kernel-space access paths from user-space virtual memory management:

  - **Physical Layer (Buddy System):** Physical memory is managed by a **Buddy System allocator** (`buddy.c`), replacing the earlier bitmap-only design. This allows scalable page allocation/merge operations and supports large-memory configurations up to the practical 32-bit x86 limit.
  - **Kernel Low Memory (Direct Mapping):** The kernel no longer allocates ordinary low-memory virtual addresses through a bitmap-managed kernel-vaddr pool. Instead, usable low memory is permanently mapped into the kernel higher-half direct-map window, allowing the kernel to access lowmem pages by simple address translation.
  - **Kernel High Memory Access (`kmap` / `kunmap`):** Physical pages outside the direct-mapped lowmem range are treated as high memory and are accessed through temporary mappings in a dedicated `kmap` region. This keeps the kernel model simple while still allowing the system to use memory beyond the directly mapped lowmem window.
  - **User Virtual Space (VMA Framework):** User-space virtual memory is managed through a **VMA (Virtual Memory Area)** framework rather than through per-process virtual bitmaps.
    - **Demand Paging:** ELF segments, user heap, stack growth, anonymous mappings, and file-backed mappings are all handled lazily. Physical pages are only allocated and mapped when a Page Fault is triggered by actual access.
    - **Dynamic Management:** Supports VMA insertion, merging, splitting, and gap searching. This provides the infrastructure required for `brk`, `mmap`, `munmap`, lazy file mapping, and Copy-On-Write (COW) during `fork`.
    - **mmap Layout Policy:** User `mmap` regions are allocated from the high user address range downward (below the reserved stack window), avoiding conflicts with the `brk`-managed heap.
  - **Kernel Small Allocation:** Kernel small-object allocation still uses an **Arena allocator** built on top of the Buddy System. Arena metadata has been externalized into `struct page`, allowing arena payload pages to remain fully usable.
  - **User-Space Heap Allocation:** Native user-space `malloc/free` no longer rely on a kernel-side `umalloc` path. Small allocations are managed in user space through `sbrk + arena`, while large allocations are backed by `mmap/munmap`.

- **VFS Implementation & "Everything is a File" Philosophy**

  This project implements a **Virtual File System (VFS)** layer, which attempts to provide a unified interface for hardware access, inter-process communication, and disk storage.

  - **Multi-Filesystem Support**: Uses a function-pointer dispatch design. By implementing `struct super_operations`, `inode_operations`, and `file_operations`, the VFS isolates specific filesystem logic. Currently, it supports both a custom **SIFS** (Simple Index FS) and the **Ext2** specification, allowing different partitions to run different filesystems concurrently.
  - **Mount Mechanism (Mount/Umount)**: Provides basic mounting functionality. By introducing "tunneling" pointers (`i_mount` / `i_mount_at`) within the `struct inode`, the path resolution logic can traverse partition boundaries, supporting nested mount points.
  - **Dynamic I/O Dispatching**: System calls (such as `read`, `write`, and `ioctl`) bind to specific operation sets during the file-opening phase based on the Inode type, reducing the need for hardcoded type-checking:
    - **Files & Directories**: Data block addressing and metadata I/O are handled by the specific Ext2 or SIFS drivers.
    - **Inter-Process Communication (IPC)**: Anonymous pipes and named pipes (FIFOs) are integrated into the standard file descriptor (FD) management logic.
    - **Device Management**: Character devices (TTY) and block devices (IDE disks) are mapped as file nodes, accessible via the unified FD interface.
  - **Metadata Management**: Maintains a global **Inode Hash Table** as a memory cache to ensure the uniqueness of Inode instances. This supports basic features like path backtracking (`getcwd`) and file renaming (`rename`), while ensuring cache consistency during deletion operations like `unlink`.

- **Task Scheduling**: Implements a **Round-Robin** scheduling algorithm. In a design reminiscent of Linux 0.12, a task's `priority` directly determines its allocated clock **ticks**. The system decrements the current task's remaining time slice during timer interrupts, triggering a reschedule only when ticks are exhausted. This non-preemptive approach maintains simplicity while distributing CPU time based on task priority.

- **Signal System**: Provides a basic signal subsystem (supporting `SIGINT`, `SIGKILL`, `SIGCHLD`, `SIGSEGV`, etc.). By manually constructing **user-mode stack frames** within the kernel, the system enables "upcalls" to user-defined handlers. Execution context is restored via `sys_sigreturn`, allowing for custom signal handling logic.

- **Resource Recovery**: Implements a two-stage resource reclamation logic. `sys_exit` releases immediate process-private resources (such as FDs), while the destruction of core structures like page directories and kernel stacks is deferred to the parent process during `wait`/`waitpid`. This ensures stable process state synchronization and memory safety.



## 📞 System Calls

**Task Management:**

- `fork()`: Creates a child process utilizing Copy-On-Write (COW).
- `execve()`: Parses ELF files, builds the initial user stack, and registers VMAs to support on-demand loading. The current interface already reserves the `envp` slot for future environment-variable support.
- `waitpid()` / `exit()`: Handles process lifecycle synchronization and resource recycling.
- `setpgid()` / `getpgid()`: Provides basic process group management for shell job control.
- `alarm()` / `pause()`: Supports simple timed signal delivery and process suspension.

**File System & IPC:**

- `open()`, `read()`, `write()`, `close()`: POSIX-like file operations.
- `mount()` / `umount()`: Implements UNIX-like VFS mounting/unmounting, establishing "tunnels" between parent directories and child partition roots.
- `pipe()`: Creates Linux-style anonymous pipes for parent-child communication.
- `mkfifo()`: Creates persistent named pipe nodes in the file system for unrelated process IPC.
- `mknod()`: Supports the creation of special files or device nodes (e.g., in `/dev`).
- `dup2()`: Handles file descriptor redirection, essential for shell pipe implementation.
- `ioctl()`: A unified device-control interface, currently used for TTY and block-device style control requests.

**Signal Handling:**

- `sigaction()` / `signal()`: Registers signal handlers and configures signal blocking masks.
- `kill()`: Dispatches signals to specific processes or process groups.
- `sigprocmask()` / `sigpending()`: Manages the signal blocking state of a process.
- `sys_sigreturn()`: *(Internal)* Restores execution context after a user-mode signal handler returns.

**Memory Management:**

- `brk()` / `sbrk()`: Provides user heap growth/shrink support through the VMA-based heap region.
- `mmap()` / `munmap()`: Supports anonymous private mappings and file-backed private mappings, both handled lazily through the page-fault path.
- Native user-space `malloc()` / `free()`: Small allocations use `sbrk + arena`; large allocations use `mmap`.

**Linux ABI Compatibility (musl-oriented):**

- MagicBox now reserves interrupt `0x80` as a Linux i386 syscall compatibility entry, while the native MagicBox syscall ABI uses interrupt `0x77`.
- A compatibility interceptor currently translates the Linux-style syscalls already observed from musl-compiled test programs, including:
  - `getpid`
  - `exit` / `exit_group`
  - `writev`
  - `ioctl`
  - `brk`
  - `mmap2`
  - `munmap`
  - `open`
  - `write`
  - `read`
  - `close`
  - `madvise` *(currently treated as a compatibility hint / no-op)*

**Storage & Recovery:**

- ~~readraw()~~: *(Deprecated)* Previously used for raw sector access; now superseded by the unified device-file interface, aligning with the "Everything is a File" philosophy.
- `open("/dev/...", ...)`: Replaces legacy raw-disk access. All block devices (e.g., `sda`, `sdb1`) are abstracted as files, enabling user-space tools like `mkfs` and `hexdump` to operate via standard I/O calls.
  - *Example*: `open("/dev/sda", O_RDONLY)` now provides raw disk access without requiring extra dedicated kernel support.



## 📂 Project Structure

The project follows a modular design, separating kernel core logic, hardware drivers, compatibility glue, and user-space applications. Below is an overview of the current directory hierarchy:

```text
.
├── boot/               # MBR & kernel loader (bootstrapping and entry to protected mode)
├── device/             # Hardware drivers (TTY, keyboard, IDE, IOCTL, timer, console)
├── fs/                 # VFS layer, file tables, pipes/FIFOs, generic FS logic
│   ├── ext2/           # Ext2-specific super/inode/file operations
│   ├── sifs/           # SIFS-specific super/inode/file operations
│   └── *.c             # VFS and filesystem-independent operations
├── glue/               # ABI / compatibility glue code (e.g. Linux-style syscall interception)
├── include/            # Header files organized by scope
│   ├── arch/           # Architecture-related low-level headers
│   ├── linux/          # Imported Linux compatibility headers (e.g. i386 syscall numbers)
│   ├── magicbox/       # Kernel-private subsystem headers
│   ├── sys/            # Standard C-style headers used inside the project
│   └── uapi/           # User-kernel ABI definitions (types, ioctls, syscall-facing layouts)
├── kernel/             # Kernel core (initialization, interrupts, debug, signals)
├── lib/                # Shared support code
│   ├── kernel/         # Kernel-mode utility libraries
│   └── user/           # Native MagicBox user-space wrappers / allocator code
├── mm/                 # Memory management (buddy allocator, VMA, paging, swap/fault handling)
├── prog/               # User-space applications and tests
│   ├── musl_test/      # Linux-ABI / musl-oriented compatibility tests
│   ├── native_test/    # Tests written against MagicBox's native userspace ABI
│   └── shell/          # Interactive shell implementation
├── thread/             # Thread scheduler and synchronization primitives
├── userprog/           # Process and userspace management (exec, fork, wait/exit, TSS, syscalls)
├── tool/               # Development helper tools and host-side helper scripts
├── disk_env/           # Virtual disk images
├── doc/                # Documentation and preview images
├── build/              # Build artifacts
├── makefile            # Master build script
├── init_disk.sh        # Disk image / partition setup script
└── install_apps.sh     # Native user-app deployment script
```



## 🚀 How to Run

### 1. Prerequisites

Ensure you have the following tools installed on your Linux system:

- **QEMU**: The emulator used to run the OS.
- **fdisk**: Used by the scripts to partition the virtual hard disk.
- **GCC & NASM**: To compile the C and Assembly source code.

### 2. Environment Setup & Build

Follow these steps in the project root directory:

(1) **Initialize the virtual disk environment** (create images and partitions in the `./disk_env` directory)

```shell
sh init_disk.sh
```

(2) By default, the build process does not format the disk. The kernel will automatically initialize a **SIFS** partition upon the first boot. Alternatively, you can pre-format the disk as **Ext2**:

```shell
# Option A: Standard build (No host-side formatting)
# The kernel will detect the missing filesystem at boot and 
# automatically initialize a SIFS partition.
make all

# Option B: Pre-format the root partition as Ext2
# This uses the host's mkfs.ext2 tool to prepare the image.
make all EXT2=1
```

*Note: Using `EXT2=1` requires `sudo` privileges on the host for `losetup` and `mkfs.ext2` operations.*

(3) **Install User Applications** Compile and deploy shell utilities into the disk image:

```shell
sh install_apps.sh
```

### 3. Launching the OS

Navigate to the `disk_env/` directory. You can choose between a standard run or a high-fidelity hardware simulation.

#### **Standard Run**

```bash
qemu-system-i386 \
  -m 32 \
  -drive file=hd60M.img,format=raw,index=0,media=disk \
  -drive file=hd80M.img,format=raw,index=1,media=disk
```

If you want to use the third disk `sdc`, try:

```shell
qemu-system-i386 \
  -m 32 \
  -drive file=hd60M.img,format=raw,index=0,media=disk \
  -drive file=hd80M.img,format=raw,index=1,media=disk \
  -drive file=hd20M_share.img,format=raw,index=2,media=disk
```

#### **Hardware Simulation Mode**

To better simulate real hardware behavior (including sync disk I/O and precise clocking), use the following:

```bash
qemu-system-i386 \
  -m 32 \
  -drive file=hd60M.img,format=raw,index=0,media=disk,cache=directsync \
  -drive file=hd80M.img,format=raw,index=1,media=disk,cache=directsync \
  -rtc base=localtime,clock=vm \
  -icount shift=auto,sleep=on \
  -boot c
```

> **Note**:
>
> - `hd60M.img`: Contains the MBR, Loader, and the Kernel.
> - `hd80M.img` / `hd20M_share.img` : Secondary data disks. To use the partitions within these disks, they must be initialized (via `mkfs.sifs` or `mkfs.ext2`) and then attached to the VFS tree using the `mount` command.



**Memory Support:** Thanks to the newly implemented **Buddy System**, the `-m` parameter now supports up to **2048** (2GB).
