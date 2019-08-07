//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>
#include "../inc/file.h"

int sys_dup(void) {
  // LAB1
  int fd_old;

  if (argint(0, &fd_old) < 0) {
    return -1;
  }

  // Check if file descriptor is valid
  if (fd_old >= NOFILE || fd_old < 0) {
    return -1;
  }

  struct file_info* fp = myproc()->files[fd_old];
  if (fp == NULL) {
    return -1;
  }

  // Find an open slot in the process file table
  int fd;
  for (fd = 0; fd < NOFILE; fd++) {
    if (myproc()->files[fd] == NULL) break;
  }
  if (fd == NOFILE){
    return -1;
  }

  // Duplicate the file in the process table
  myproc()->files[fd] = fp;
  filedup(fp);

  return fd;
}

int sys_read(void) {
  // LAB1
  int fd;
  char *buf;
  int n;

  // Reading & checking parameters
  if (argint(0, &fd) < 0) {
    return -1;
  }
  if (argint(2, &n) < 0 || n <= 0) {
    return -1;
  }
  if (argptr(1, &buf, n) < 0) {
    return -1;
  }

  // Check if file descriptor is valid
  if (fd >= NOFILE || fd < 0) {
    return -1;
  }

  // Get the file info and check permissions
  struct file_info* fp = myproc()->files[fd];
  if (fp == NULL || (fp->perm != O_RDONLY && fp->perm != O_RDWR)) {
    return -1;
  }

  return fileread(fp, buf, n);
}

int sys_write(void) {
  int fd;
  char *buf;
  int n;

  // Reading & checking parameters
  if (argint(0, &fd) < 0) {
    return -1;
  }
  if (argint(2, &n) < 0 || n <= 0) {
    return - 1;
  }
  if (argptr(1, &buf, n) < 0) {
    return -1;
  }

  // Check if file descriptor is valid
  if (fd >= NOFILE || fd < 0) {
    return -1;
  }

  // Get the file info and check permissions
  struct file_info* fp = myproc()->files[fd];
  if (fp == NULL || (fp->perm != O_WRONLY && fp->perm != O_RDWR)) {
    return -1;
  }

  return filewrite(fp, buf, n);
}

int sys_close(void) {
  int fd;

  // Reading & checking parameters
  if (argint(0, &fd) < 0) {
    return -1;
  }
  if (fd >= NOFILE || fd < 0) {
    return -1;
  }

  // Get the file info
  struct file_info* fp = myproc()->files[fd];
  if (fp == NULL) {
    return -1;
  }

  fileclose(fp);

  // Close the file in the process
  myproc()->files[fd] = NULL;
  return 0;
}

int sys_fstat(void) {
  int fd;
  struct stat *sp;

  // Reading & checking parameters
  if (argint(0, &fd) < 0) {
    return - 1;
  }
  if (fd >= NOFILE || fd < 0) {
    return - 1;
  }
  if (argptr(1, (char**) &sp, sizeof(struct stat))) {
    return -1;
  }

  // Get the file info
  struct file_info* fp = myproc()->files[fd];
  if (fp == NULL) {
    return - 1;
  }

  // Get the inode
  struct inode* ip = fp->ip;
  if (ip == NULL) {
    return -1;
  }

  // Set stat struct to contain inode values
  sp->type = ip->type;
  sp->dev = ip->dev;
  sp->ino = ip->inum;
  sp->size = ip->size;
  return 0;
}

int sys_open(void) {
  // LAB1
  char* path;
  int mode;

  if (argstr(0, &path) < 0) {
    // points to an invalid or unmapped address
    // there is an invalid address before the end of the string 
    return -1;
  }
  if (argint(1, &mode) < 0 || mode == O_CREATE) // read only
    return -1;

  // find an open slot in the process file table
  int fd;
  for (fd = 0; fd < NOFILE; fd++) {
    if (myproc()->files[fd] == NULL) break;
  }
  if (fd == NOFILE) return -1;

  // Finds an open entry in the global open file table
  struct file_info* fp = fileopen(path, mode);
  if (fp == NULL) return -1;
  
  myproc()->files[fd] = fp;
  return fd;
}

int sys_exec(void) {
  // LAB2
  char* path;
  char** argv;

  if (argstr(0, &path) < 0) {
    // points to an invalid or unmapped address
    // there is an invalid address before the end of the string 
    return -1;
  }

  if (argptr(1, (char**)&argv, 8) < 0)
    return -1;

  return exec(path, argv);
}

int sys_pipe(void) {
  // LAB2
  int* fd_arr;
  // Reading & checking parameter
  if (argptr(0, (char**) &fd_arr, sizeof(int) * 2) < 0) {
    return - 1;
  }

  struct pipe* pipe;

  if((pipe = (struct pipe*)kalloc()) == 0) {
    return -1;
  }

  // Reassign the pipe address to be at the start of the page, and move
  // the head and tail of the buffer to the end of the struct
  pipe->head += sizeof(struct pipe);
  pipe->tail += sizeof(struct pipe);

  pipe->buf = (char*) pipe;

  // Initialize pipe lock
  initlock(&(pipe->lock), "pipe spinlock");

  // Set pipe read and write to open
  pipe->hasopenread = true;
  pipe->hasopenwrite = true;

  int rfd, wfd;

  // Open the pipe read fd
  rfd = pipeopen(pipe, O_RDONLY);
  if (rfd == -1) {
    return -1;
  }

  // Open the pipe write fd
  wfd = pipeopen(pipe, O_WRONLY);
  if (wfd == -1) {
    return -1;
  }

  fd_arr[0] = rfd;
  fd_arr[1] = wfd;

  return 0;
}
