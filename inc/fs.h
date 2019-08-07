#pragma once

#include "extent.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define INODEFILEINO 0 // inode file inum
#define ROOTINO 1      // root i-number
#define BSIZE 512      // block size
#define SWAPPAGES 2048 // number of swap pages

// Disk layout:
// [ boot block | super block | free bit map |
//                                          inode file | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;       // Size of file system image (blocks)
  uint nblocks;    // Number of data blocks
  uint bmapstart;  // Block number of first free map block
  uint inodestart; // Block number of the start of inode file
  uint swapstart;  // Block number of the start of swap region
  uint logstart;   // Block number of the start of log region
};

// On-disk inode structure
struct dinode {
  short type;         // File type
  short devid;        // Device number (T_DEV only)
  uint size;          // Size of file (bytes)
  struct extent data[6]; // Data blocks of file on disk
  char pad[6];       // So disk inodes fit contiguosly in a block
};

// offset of inode in inodefile
#define INODEOFF(inum) ((inum) * sizeof(struct dinode))

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

struct log_meta {
    short commited; // Whether log changes are commited
    uint nchanges; // Number of changes (Also the index of the last change)
    uint blocknos[19]; // Array of block that the changes are written to
};
