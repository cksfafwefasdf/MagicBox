# MagicBox
**MagicBox** is an early-stage 32-bit x86 operating system prototype. This project serves as a personal practice work to explore the fundamental principles of operating systems, based on classic textbooks and community implementations. It replicates the core pipeline from initial bootloading to a basic user-mode environment.

> **Note**: This project is intended for learning purposes only, utilizing the traditional 32-bit Protected Mode tech stack.

![MagicBox Preview](./doc/pic/preview.png)

## 📚 Learning Objectives

- **Bootstrapping & Environment**: Implemented a minimal MBR and Loader to handle the transition from Real Mode to 32-bit Protected Mode, along with an initial implementation of basic paging.
- **Minimalist Process Model**: Explored Unix-style process lifecycle management by replicating core system calls such as `fork()` and `execv()`.
- **Experimental App Deployment**: To simplify the deployment process, a simple **Tar-based** archiving mechanism was implemented. Binaries are written to raw disk sectors and extracted by the kernel into the file system during boot.
- **Basic Interaction**: Developed a rudimentary command-line Shell that supports internal commands and simple path resolution for external programs (e.g., searching in `/bin`).



## 🛠 Technical Overview

- **Interrupts & Sync**: Handles hardware interrupts via the **8259A PIC** and implements thread synchronization using **mutexes and semaphores**.
- **Memory Management**: A dual-pool (Kernel/User) system using **bitmaps** for page allocation, featuring an **Arena-based allocator** for fine-grained memory requests and recursive page table mapping.
- **Unix-like File System**: An **Inode-based** FS supporting hierarchical directories, file descriptors, and IPC via **Pipes**.
- **Task Scheduling**: A **none-preemptive Round-Robin scheduler** where task priority determines the duration of its **time slice**. Tasks are preempted once their allocated clock ticks expire.



## 📞 System Calls

The kernel provides a variety of system calls to bridge user applications and kernel services. Key implementations include:

- **Process Management**:
  - `fork()`: Create a child process by duplicating the caller.
  - `execv()`: Load and execute a new program.
  - `wait()` & `exit()`: Handle process synchronization and resource reclamation.
- **File System & I/O**:
  - `open()`, `read()`, `write()`, `close()`: Standard POSIX-like file operations.
  - `pipe()`: Support for inter-process communication.
  - `fd_redirect()`: Facilitates I/O redirection (essential for shell pipe implementation).
- **Memory Management**:
  - `malloc()` & `free()`: Dynamic memory allocation for user-space programs.
- **Storage & Recovery (Experimental)**:
  - `readraw()` & `mount()`: Direct disk access and partition mounting.



## 📂 Project Structure

The project follows a modular design, separating kernel core logic, hardware drivers, and user-space applications. Below is an overview of the directory hierarchy:

```
.
├── boot/               # MBR & Kernel Loader (Entry point to protected mode)
├── device/             # Hardware Drivers (Keyboard, IDE, Timer, Console)
├── fs/                 # File System (Inode, Directory, and Pipe management)
├── kernel/             # Kernel Core (Interrupts, Memory management, Init)
├── lib/                # Library routines
│   ├── common/         # Shared code (e.g., Tar parsing logic)
│   ├── kernel/         # Internal kernel-mode libraries
│   └── user/           # User-mode system call wrappers
├── prog/               # User-land applications
│   ├── shell/          # Interactive Shell implementation
│   └── prog/           # Core utilities (cat, echo, etc.)
├── thread/             # Threading & Sync (Mutex, Semaphores, Scheduling)
├── userprog/           # Process Management (Exec, Fork, Wait/Exit, TSS)
├── disk_env/           # Hard disk images and disk-related configs
├── build/              # Binary artifacts and object files
├── makefile            # Master build script
├── init_disk.sh        # Disk image & partition setup script
└── install_apps.sh     # App packaging (Tar) and deployment script
```



## 🚀 How to Run

### 1. Prerequisites

Ensure you have the following tools installed on your Linux system:

- **QEMU**: The emulator used to run the OS.
- **fdisk**: Used by the scripts to partition the virtual hard disk.
- **GCC & NASM**: To compile the C and Assembly source code.

### 2. Environment Setup & Build

Follow these steps in the project root directory:

```shell
# Step 1: Initialize the virtual disk environment (create images and partitions)
sh init_disk.sh

# Step 2: Build the kernel and bootloader
make all

# Step 3: Compile user applications and install them into the disk (via Tar)
sh install_apps.sh
```

### 3. Launching the OS

Navigate to the `disk_env/` directory and execute the following command to start MagicBox in QEMU:

```shell
cd disk_env

qemu-system-i386 \
  -m 32 \
  -drive file=hd60M.img,format=raw,index=0,media=disk \
  -drive file=hd80M.img,format=raw,index=1,media=disk
```

> **Note**:
>
> - `hd60M.img` contains the MBR, Loader, and the Kernel.
> - `hd80M.img` is the data disk where your file system and Tar-packaged apps reside.



## ⏰ TODO List

### **Project Status**

This is a personal learning project currently in its early experimental stages. The following plans represent preliminary goals for functional exploration:

- **P0**: Foundational compatibility. Attempting to provide basic support for early utilities like **BusyBox v0.60**.
- **P1**: Architectural refinement. Trying to establish a simple environment to support self-hosting experiments with **GCC 1.40 / 2.7.2**.
- **P2**: Feature exploration. Preliminary attempts at simple networking and basic interface improvements.

------

### 🧠 Memory Management (MM)

- ~~**Internal Arena Allocator**: Basic kernel-space memory allocation implemented.~~
- ~~**Legacy `malloc` Syscall**: Simple direct kernel-to-user memory allocation implemented.~~
- **[P0] Heap Management (`brk`)**: Implement a simple `sys_brk` for basic user-space heap boundary control.
- **[P1] Copy-On-Write (COW)**: Attempt to implement simple COW logic to mitigate physical memory overhead during `fork()`.
- **[P1] Memory Mapping (`mmap`)**: Preliminary support for basic file-backed and anonymous memory mapping.
- **[P2] Buddy System**: Try to implement a simple page-level allocator to explore physical memory management.
- **[P2] Slab Allocator**: Attempt to implement a basic object-level cache for common kernel structures.

### 📁 File System (FS) & I/O

- ~~**Basic VFS**: Preliminary implementation of `open`, `read`, `write`, `close`, `lseek`.~~
- ~~**Directory Operations**: Basic support for `mkdir`, `rmdir`, and `mount`.~~
- ~~**Buffer Cache**: Simple block-level caching mechanism implemented.~~
- ~~**Basic Pipe**: Support for unidirectional pipes and simple I/O redirection.~~
- **[P0] POSIX FS Refinement**:
  - Expand `struct stat` to include basic members like `st_mode` and `st_nlink`.
  - Implement a basic `sys_getdents` for simple directory traversal.
  - Handle basic `i_nlink` synchronization for directories and files.
- **[P1] FS Robustness & Scaling**:
  - **Indirect Block Validation**: Verify and refine the logic for `i_sectors[12]` to support larger files (>6KB).
  - Implement a basic `sys_dup2` for common file descriptor duplication.
- **[P2] DMA Driver**: Preliminary attempt at a simple DMA driver for disk I/O to reduce CPU involvement.

### ⚡ Process & Scheduling

- ~~**Task Control**: Basic `fork`, `execv`, and `exit` flow implemented.~~
- ~~**Scheduler**: Simple priority-based Round-Robin (non-preemptive).~~
- **[P0] Process Synchronization**: Refine `sys_waitpid` (or the existing `wait`) to collect basic child exit status.
- **[P0] Signal Mechanism**: Support for basic signal handling (e.g., simple `SIGINT`, `SIGCHLD`).
- **[P1] Environment Support**: Support passing basic environment variables (e.g., `PATH`) during `execve`.
- **[P1] Preemptive Scheduling**: Attempt a basic preemption logic within the timer interrupt for better responsiveness.

### 💻 Console & UX

- ~~**Basic Console**: Simple VGA text mode input and output implemented.~~
- **[P1] Interactive UX**: Support for basic ANSI escape sequences to enable arrow keys and simple command history.
- **[P1] Scrollback Support**: Implementation of a basic VGA buffer scrollback for `PgUp`/`PgDn` support.
- **[P2] Network Stack**: Preliminary exploration of basic network protocols (e.g., ARP, ICMP, and UDP).

------

### 🏁 Milestones (Experimental Goals)

1. **Milestone 1**: Attempt to execute unmodified **BusyBox v0.60** core binaries.
2. **Milestone 2**: **Self-hosting Experiment** - Try to compile basic MagicBox kernel source code using **GCC/Make** within the OS environment.

