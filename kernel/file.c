//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <proc.h>
#include "../inc/file.h"

#include <fcntl.h>
#include <stat.h>

struct devsw devsw[NDEV];

struct file_info files_global[NFILE];

/*
 * Updates the reference count for the file represented by fp.
 */
void filedup(struct file_info* fp) {
  acquiresleep(&(fp->lock));
  fp->ref++;
  releasesleep(&(fp->lock));
}

/*
 * Write n-bytes of content from the buf to the file represented by fp, and
 * return the number of bytes written.
 */
int filewrite(struct file_info* fp, char* buf, int n) {
  // Check if file_info is a pipe
  if (fp->is_pipe) {
    return pipewrite(fp, buf, n);
  }

  acquiresleep(&(fp->lock));
  int bytes = concurrent_writei(fp->ip, buf, fp->offset, n);
  if (bytes == -1) {
    return -1;
  }

  fp->offset += bytes;
  releasesleep(&(fp->lock));

  return bytes;
}

/*
 * Read n-bytes of content from file represented by fp and store it in buf,
 * and return the number of bytes read.
 */
int fileread(struct file_info* fp, char* buf, int n) {
  // Check if file_info is a pipe
  if (fp->is_pipe) {
    return piperead(fp, buf, n);
  }

  int bytes = concurrent_readi(fp->ip, buf, fp->offset, n);
  if (bytes == -1) {
    return -1;
  }

  acquiresleep(&(fp->lock));
  fp->offset += bytes;
  releasesleep(&(fp->lock));

  return bytes;
}

/**
 * Find and open spot in the global file table and allocates a new file_info
 * struct to store in that spot. Return the newly allocated file_info struct.
 */
struct file_info* fileopen(char* path, int mode) {
	struct inode *ip = namei(path);
  if (ip == 0) {
    if (mode < O_CREATE || addfile(path) < 0) {
      return NULL; // file does not exist
    }
    ip = namei(path);
    assert(ip != NULL);
  }
  if (mode >= O_CREATE)
    mode -= O_CREATE;

  // Finds an open entry in the global open file table
  for (int i = 0; i < NFILE; i++) {
    if (files_global[i].ip == NULL && files_global[i].pp == NULL) {
      // allocates a new file_info struct
      struct file_info finfo;
      initsleeplock(&(finfo.lock), "open sleeplock");
      acquiresleep(&(finfo.lock));

      finfo.ip = ip;
      finfo.ref = 1;
      finfo.offset = 0;
      finfo.perm = mode;
      finfo.is_pipe = false;
      finfo.pp = NULL;

      files_global[i] = finfo;
      releasesleep(&(files_global[i].lock));
      return files_global + i;
    }
  }
  return NULL;
}

/**
 * Release the file from the global file table.
 */
void fileclose(struct file_info* fp) {
  acquiresleep(&(fp->lock));
  fp->ref--;
  // Only close file in global file table if reference count is 0
  if (fp->ref == 0) {
    if (fp->is_pipe) {
      pipeclose(fp);
    } else {
      irelease(fp->ip);
      fp->ip = NULL;
      fp->offset = 0;
    }
  }
  releasesleep(&(fp->lock));
}

/*
 * Get stats about the file.
 */
void filestat(struct file_info* fp, struct stat *sp) {
	// Get the inode
  acquiresleep(&(fp->lock));
  struct inode* ip = fp->ip;

  // Set stat struct to contain inode values
  concurrent_stati(ip, sp);
  releasesleep(&(fp->lock));
}

/*
 * Open either the pipe read or write file descriptor based on mode.
 */
int pipeopen(struct pipe* pipe, int mode) {
  int fd;
  for (fd = 0; fd < NOFILE; fd++) {
    if (myproc()->files[fd] == NULL) {
      break;
    }
  }

  if (fd == NOFILE) {
    return -1;
  }

  // Finds an open entry in the global open file table
  struct file_info finfo;
  for (int i = 0; i < NFILE; i++) {
    if (files_global[i].ip == NULL && files_global[i].pp == NULL) {
      // allocates a new file_info struct
      initsleeplock(&(finfo.lock), "pipeopen sleeplock");
      acquiresleep(&(finfo.lock));

      finfo.pp = pipe;
      finfo.is_pipe = true;
      finfo.ref = 1;
      finfo.offset = 0;
      finfo.perm = mode;
      finfo.ip = NULL;

      files_global[i] = finfo;
      myproc()->files[fd] = files_global + i;
      releasesleep(&(files_global[i].lock));

      return fd;
    }
  }
  return -1;
}

/*
 * Read up to n bytes of data from the pipe and store them in buf.
 */
int piperead(struct file_info* fp, char* buf, int n) {
  int buf_size = PGSIZE - sizeof(struct pipe);
  int num_read;

  struct pipe* pipe = fp->pp;
  acquire(&(pipe->lock));

  while (pipe->head == pipe->tail) {
    if (pipe->hasopenwrite) {
      // Sleep until data is available to read
      sleep((void*)&pipe->hasopenwrite, &(pipe->lock));
    } else {
      // EOF
      release(&(pipe->lock));
      return 0;
    }
  }

  // Read the buffered data
  if ((pipe->tail - pipe->head) < n) {
    // Read tail minus head bytes from pipe and copy into buf
    memmove((void *) buf, (void *) pipe->buf + (pipe->head % buf_size),
        pipe->tail - pipe->head);

    num_read = pipe->tail - pipe->head;   // Record num bytes read
    pipe->head += num_read;              // Update head of buffer
  } else {
    // Read n bytes from pipe and copy into buf
    memmove((void *) buf, (void *) pipe->buf + (pipe->head % buf_size), n);

    num_read = n;     // Record num bytes read
    pipe->head += n;  // Update head of buffer
  }
  release(&(pipe->lock));

  // Call wakeup to signal buffer is no longer full
  wakeup((void*)&pipe->hasopenread);

  return num_read;
}

/*
 * Write up to n bytes of data from buf and store them in the pipe.
 */
int pipewrite(struct file_info* fp, char* buf, int n) {
  int buf_size = PGSIZE - sizeof(struct pipe);
  int num_written;

  struct pipe* pipe = fp->pp;
  acquire(&(pipe->lock));

  // Return error if trying to write with no read fd open
  if (!pipe->hasopenread) {
    release(&(pipe->lock));
    return -1;
  }

  // Sleep until there is room to write data
  while (pipe->tail - pipe->head == buf_size) {
    sleep((void*)&pipe->hasopenread, &(pipe->lock));
  }

  // Write the buffered data
  if (buf_size - (pipe->tail % buf_size) < n) {
    // Case 1: Not enough room at end of buffer, need to circle back around

    // Number of byes from start of buffer to the current head
    int start_to_head = pipe->head % buf_size;
    // Number of bytes from the current tail to the end of the buffer
    int tail_to_end = buf_size - (pipe->tail % buf_size);

    // Fill the end of the buffer
    memmove((void *) pipe->buf + (pipe->tail % buf_size), (void *) buf,
        tail_to_end);

    num_written = tail_to_end;
    pipe->tail += num_written;
    int remaining = n - tail_to_end;

    // Circle back to the front of the buffer and write as much as possible
    if (remaining < start_to_head) {
      // Write all the remaining bytes
      memmove((void *) pipe->buf, (void *) buf, remaining);

      num_written += remaining;
      pipe->tail += remaining;
    } else {
      // Write as much as possible up to the head
      memmove((void *) pipe->buf, (void *) buf, start_to_head);

      num_written += start_to_head;
      pipe->tail += start_to_head;
    }

  } else {
    // Case 2: There is enough room at the end of the buffer, rite n bytes
    memmove((void *) pipe->buf + (pipe->tail % buf_size), (void *) buf, n);

    num_written = n;
    pipe->tail += n;
  }
  release(&(pipe->lock));

  // Call wakeup to signal data can be read
  wakeup((void*)&pipe->hasopenwrite);

  return num_written;
}

/*
 * Close a given file descriptor for the pipe, and close the entire pipe if
 * both the read and write fd's are closed.
 */
void pipeclose(struct file_info* fp) {
  struct pipe* pipe = fp->pp;
  acquire(&(pipe->lock));

  if (fp->perm == O_RDONLY) {
    pipe->hasopenread = false;
    wakeup((void*)pipe->hasopenread);
  } else if (fp->perm == O_WRONLY) {
    pipe->hasopenwrite = false;
    wakeup((void*)pipe->hasopenwrite);
  } else {
    panic("permission is neither read or write only");
  }
  release(&(pipe->lock));

  if (!pipe->hasopenread && !pipe->hasopenwrite) {
    kfree(pipe->buf);
  }
}






