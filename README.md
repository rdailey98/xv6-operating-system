## Overview
All skeleton code is credited to MIT. More documentation and info can be found here: https://github.com/mit-pdos/xv6-public <br/>
xv6 is a lightweight operating system developed by MIT to help in OS learning. All system calls and simple OS functionallity in this project are originally stubbed out and left for the developer to fill in. Upon completing the project, I added the following capabilities to the operating system:
- Kernel interrupts
- System calls (read, write, open, fork, exec, etc..)
- Functioning file system
- Multiprocessing
- Address space management
- Page swapping
- Crash safety

## Skills and Tools Gained
- C programming language
- Concurency management
- Mutliprocessing system design
- Locking (mutex, spin locks, sleep locks, etc..)
- Debugging
- Unit testing

### Running the OS

This projects uses the [QEMU Emulator](http://www.qemu.org/), a
modern and fast emulator. While QEMU's built-in monitor
provides only limited debugging support, QEMU can act as a remote
debugging target for the GNU debugger (GDB).

To get started, clone the repo and extract the source code into your own directory, then type
`make` in the root directory to build this application.

Now you're ready to run QEMU, supplying the file `out/xk.img` and `out/fs.img`,
created above, as the contents of the emulated PC's "virtual hard
disk." Those hard disk images contain our boot loader `out/bootblock`
, our kernel `out/xk.bin` and a list of user applications in `out/user`.

Type `make qemu` to run QEMU with the options required to
set the hard disk and direct serial port output to the terminal.
Some text should appear in the QEMU window:

```
xk...
CPU: QEMU Virtual CPU version 2.5+
  fpu de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2
  sse3 cx16 hypervisor
  syscall nx lm
  lahf_lm svm
E820: physical memory map [mem 0x9000-0x908f]
  [mem 0x0-0x9fbff] available
  [mem 0x9fc00-0x9ffff] reserved
  [mem 0xf0000-0xfffff] reserved
  [mem 0x100000-0xfdffff] available
  [mem 0xfe0000-0xffffff] reserved
  [mem 0xfffc0000-0xffffffff] reserved

cpu0: starting xk

free pages: 3741
cpu0: starting
sb: size 100000 nblocks 99973 bmap start 2 inodestart 27
hello world
```

Press Ctrl-a x to exit the QEMU virtual instance.

### GDB

GDB can be used as a remote debugger. Before you can start using GDB, you have to add our given .gdbinit to be allowed on your machine. Create a file `~/.gdbinit` in your home directory and add the following line:
```
add-auto-load-safe-path /absolute/path/to/xk/.gdbinit
```
(where `/absolute/path/to/xk/` is the absolute path to your xk directory)

To attach GDB to the project, you need to open two separate terminals. Both of them should be in the root directory. In one terminal, type `make qemu-gdb`. This starts the qemu process and wait for GDB to attach. In another terminal, type `make gdb`. Now the GDB process is attached to qemu.

In this application, when bootloader loads the kernel from disk to memory, the CPU operates in 32-bit mode. The starting point of the 32-bit kernel is in `kernel/entry.S`. `kernel/entry.S` setups 64-bit virtual memory and enables 64-bit mode.
