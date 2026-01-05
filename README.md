# R-OS Project (Codename: ROS)
## Note: ROS stands for Rasya OS, not Robot Operating System and definitely NOT ReactOS.

Welcome to the official repository of ROS. This project is a personal journey to recreate and explore Operating System concepts from scratch. Published under the GPLv2 license, this kernel is the result of my research and passion for low-level development.

It has been tested on QEMU (highly recommended) and VirtualBox (mostly stable, please report bugs if you find any).

---

# 🚀 Highlights & Usage
1. How to Build
To compile ROS, you will need the following tools:

NASM (Assembler)

x86_64-elf-gcc (GNU Cross Compiler)

Python 3 (Required for the kernsym generator)

## Steps:
* Navigate to the project root.
* Run the build command:
> make -j8
The output will be kernel.elf (the raw kernel).

> Bootloader: I recommend using GRUB with MultibootV2 support, as boot.asm is implemented following that standard. For now, you'll need to manually package it into an .iso.
> Planned feature: Support for .img output via make -j8 OUTIMG.

2. Stability Status
Is it stable? Well, I wouldn't call it 100% production-ready, but for short-term sessions (a few hours of uptime), it is quite solid. Due to the frequent recompile-and-test cycle, I haven't stress-tested it for long durations yet, but it handles basic tasks without crashing.

3. Future Roadmap (The Vision)
Currently, this repository focuses solely on the Kernel. I am actively working on a separate Userland. My current missions are:

Completing POSIX syscall implementation (it's tedious, but I'll get there!).

Developing core applications: File Explorer (Nautilus-style), a native Terminal, and TEXTPAD.
