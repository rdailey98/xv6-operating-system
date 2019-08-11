# Tools

## Toolchain Setup

### Mac Users

Requirement: Xcode >= 8.0.

brew tap xiw/crossbuild

brew install x86_64-linux-gnu-binutils

brew install x86_64-linux-gnu-gdb

### 64-bit Linux Users

Native toolchain works.

## Building QEMU

You need QEMU >= 2.9. To build it from source code please consult [QEMU instruction manual](https://www.qemu.org/download/#source).

QEMU requires libsdl. This should not be a problem with Mac users. For 64-bit Linux users, you might need to install it.

For Ubuntu, you can do the followings:

sudo apt-get install libsdl2-dev
