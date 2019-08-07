#pragma once

#include <extent.h>
#include <sleeplock.h>

// in-memory copy of an inode
struct inode {
  uint dev;  // Device number
  uint inum; // Inode number
  int ref;   // Reference count
  int valid; // Flag for if node is valid
  struct sleeplock lock;

  short type; // copy of disk inode
  short devid;
  uint size;
  struct extent data[6];
};

// table mapping device ID (devid) to device functions
struct devsw {
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

extern struct devsw devsw[];

// Device ids
enum {
  CONSOLE = 1,
};

// keep track of a logical file
struct file_info {
  struct sleeplock lock;
  struct inode* ip; // Reference to inode
  struct pipe* pp;
  bool is_pipe;
  int ref;   // Reference count
  uint offset; // Current offset in file
  int perm; // Access permission
};

extern struct file_info files_global[];

struct pipe {
  struct spinlock lock;
  char* buf;
  int head;
  int tail;
  bool hasopenread;
  bool hasopenwrite;
};