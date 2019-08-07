// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>
#include "../inc/buf.h"
#include "../inc/fs.h"

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;
struct sleeplock loglock;

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 1 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iget() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. irelease() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.



struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.valid = 1;
  icache.inodefile.ref = 1;

  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  // icache.inodefile.data = di.data;

  for (int i = 0; i < 6; i++) {
    icache.inodefile.data[i] = di.data[i];
  }

  brelse(b);
}

void iinit(int dev) {
  int i, j;
  struct log_meta log;
  struct buf *buf, *log_buf;
  uint block_no;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);

  init_inodefile(dev);

  initsleeplock(&loglock, "loglock");

  // Read log_meta in
  buf = bread(ROOTDEV, sb.logstart);
  memmove(&log, buf->data, sizeof(struct log_meta));
  brelse(buf);

  if (log.commited == 1) {
    // Write log blocks to their corresponding place on disk
    for (j = 0; j < log.nchanges; j++) {
      block_no = log.blocknos[j];
      buf = bread(ROOTDEV, block_no);

      log_buf = bread(ROOTDEV, sb.logstart + 1 + j);

      memmove(buf->data, log_buf->data, BSIZE);
      bwrite(buf);
      brelse(buf);
    }

    // Set log to uncommitted and nchanges = 0, then write to disk
    memset(&log, 0, sizeof(struct log_meta));
    buf = bread(ROOTDEV, sb.logstart);
    memmove(buf->data, &log, BSIZE);
    bwrite(buf);
    brelse(buf);
  }
}


// Reads the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
static void read_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->valid = 0;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void locki(struct inode *ip) {
  struct dinode dip;

  if(ip == 0 || ip->ref < 1)
    panic("locki");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {

    if (ip != &icache.inodefile)
      locki(&icache.inodefile);
    read_dinode(ip->inum, &dip);
    if (ip != &icache.inodefile)
      unlocki(&icache.inodefile);

    ip->type = dip.type;
    ip->devid = dip.devid;

    ip->size = dip.size;
    // ip->data = dip.data;
    for (int i = 0; i < 6; i++) {
      ip->data[i] = dip.data[i];
    }

    ip->valid = 1;

    if (ip->type == 0)
      panic("iget: no type");
  }
}

// Unlock the given inode.
void unlocki(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("unlocki");

  releasesleep(&ip->lock);
}

// threadsafe stati.
void concurrent_stati(struct inode *ip, struct stat *st) {
  locki(ip);
  stati(ip, st);
  unlocki(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

// threadsafe readi.
int concurrent_readi(struct inode *ip, char *dst, uint off, uint n) {
  int retval;

  locki(ip);
  retval = readi(ip, dst, off, n);
  unlocki(ip);

  return retval;
}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  struct extent* extent = &ip->data[0];
  uint extno = 0, extoff = 0, foff = 0;
  uint tot, m;
  struct buf* buf;

  for (tot = 0; tot < n; extoff++, foff++) {
    assert(extent->nblocks > 0);
    if (extoff >= extent->nblocks) {
      // move on the next extent
      assert(extno < 5);
      extno++;
      extent = &ip->data[extno];
      extoff = 0;
    }
    if (foff >= off / BSIZE) {
      buf = bread(ip->dev, extent->startblkno + extoff);
      m = min(n - tot, BSIZE - off % BSIZE);
      memmove(dst, buf->data + off % BSIZE, m);
      brelse(buf);
      off += m;
      tot += m;
      dst += m;
    }
  }
  return n;
}

uint balloc(void) {
  int data[BSIZE / sizeof(int)];
  struct buf *buf;
  uint blockno;
  uint i;
  uint addr;

  for (blockno = BBLOCK(sb.inodestart, sb); blockno < sb.inodestart; blockno++) {
    buf = bread(ROOTDEV, blockno);
    memmove(data, buf->data, BSIZE);  // copy buf into data

    for (i = 0; i < BSIZE/sizeof(int); i++) {
      if (data[i] == 0) {  // 32 bits of 0
        data[i] = -1;  // 32 bits of 1
        memmove(buf->data, data, BSIZE);  // copy data into buf
        log_write(buf);
        brelse(buf);
        addr = (blockno - sb.bmapstart) * BPB + i * 32;
        assert(addr > sb.inodestart);
        assert(addr < FSSIZE);
        return addr;
      }
    }

    brelse(buf);
  }

  panic("No more free space in extent region");
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;
  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

// Write data to inode.
// Returns number of bytes written.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }
  
  if (off > ip->size || off + n < off) {
    return -1;
  }
  
  struct extent* extent = &ip->data[0];
  uint extno = 0, extoff = 0, foff = 0;
  uint tot, m;
  struct buf* buf;
  struct dinode dip;
  uint oldoff = off;
  short log_started = holdingsleep(&loglock);

  for (tot = 0; tot < n; extoff++, foff++) {
    if (extoff >= extent->nblocks && extent->nblocks != 0) {
      // end of this extent, move on the next extent
      extno++;
      extent = &ip->data[extno];
      extoff = 0;
    }

    if (foff >= off / BSIZE) {
      if (!log_started)
        begin_tx();

      if (extent->nblocks == 0) {
        // empty extent, allocate new blocks
        if (extno == 5)
          panic("Run out of space for a file");
        extent->startblkno = balloc();
        extent->nblocks = 32;
      }
      
      buf = bread(ip->dev, extent->startblkno + extoff);
      m = min(n - tot, BSIZE - off % BSIZE);
      memmove(buf->data + off % BSIZE, src, m);
      log_write(buf);
      brelse(buf);
      off += m;
      tot += m;
      src += m;

      // update file size
      if (oldoff + tot > ip->size) {
        ip->size = oldoff + tot;
      }

      // populate dinode
      dip.type = ip->type;
      dip.devid = ip->devid;
      dip.size = ip->size;
      for (int i = 0; i < 6; i++) {
        dip.data[i] = ip->data[i];
      }

      // write the updated dinode back to disk
      if (ip != &icache.inodefile)
        concurrent_writei(&icache.inodefile, &dip, INODEOFF(ip->inum), sizeof(struct dinode));
      else {
        buf = bread(ip->dev, sb.inodestart);
        memmove(buf->data, &dip, sizeof(struct dinode));
        log_write(buf);
        brelse(buf);
      }

      if (!log_started)
        commit_tx();
    }
  }
  return n;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

void swapread(int dev, uint swap_index, char* addr) {
  struct buf *buf;
  uint block_no;

  for (int i = 0; i < 8; i++) {
    block_no = sb.swapstart + swap_index * 8 + i;
    buf = bread(dev, block_no);
    memmove(addr, buf->data, BSIZE);
    brelse(buf);
    addr += BSIZE;
  }
}

void swapwrite(int dev, uint swap_index, char* addr) {
  struct buf *buf;
  uint block_no;

  for (int i = 0; i < 8; i++) {
    block_no = sb.swapstart + swap_index * 8 + i;
    buf = bread(ROOTDEV, block_no);
    memmove(buf->data, addr, BSIZE);
    bwrite(buf);
    brelse(buf);
    addr += BSIZE;
  }
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(namei("/"));

  while ((path = skipelem(path, name)) != 0) {
    locki(ip);
    if (ip->type != T_DIR) {
      unlocki(ip);
      goto notfound;
    }

    // Stop one level early.
    if (nameiparent && *path == '\0') {
      unlocki(ip);
      return ip;
    }

    if ((next = dirlookup(ip, name, 0)) == 0) {
      unlocki(ip);
      goto notfound;
    }

    unlocki(ip);
    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}

int addfile(char* path) {
  struct dinode di;
  uint inum;
  struct inode* rootdev;
  struct dirent dirent;

  begin_tx();

  // Initialize a new struct dinode
  di.devid = ROOTDEV;
  di.size = 0;
  di.type = T_FILE;
  di.data[0].startblkno = balloc();
  di.data[0].nblocks = 32;
  for (int i = 1; i < 6; i++) {
    di.data[i].startblkno = 0;
    di.data[i].nblocks = 0;
  }
  
  // append the dinode to the end of the inode file
  locki(&icache.inodefile);
  if (writei(&icache.inodefile, &di, icache.inodefile.size, sizeof(struct dinode))
    != sizeof(struct dinode)) {
    unlocki(&icache.inodefile);
    return -1;
  }
  inum = icache.inodefile.size / sizeof(struct dinode) - 1;
  unlocki(&icache.inodefile);

  // append a new dirent to the root directory
  rootdev = iget(ROOTDEV, ROOTINO);
  dirent.inum = inum;
  skipelem(path, dirent.name);
  locki(rootdev);
  if (writei(rootdev, &dirent, rootdev->size, sizeof(struct dirent))
    != sizeof(struct dirent)) {
    unlocki(rootdev);
    return -1;
  }
  unlocki(rootdev);
  irelease(rootdev);

  commit_tx();

  return 0;
}

void begin_tx() {
  struct log_meta log;
  struct buf *buf;

  acquiresleep(&loglock);

  // Initialize a new struct log_meta
  memset(&log, 0, sizeof(struct log_meta));

  // Write struct to beginning of log region
  buf = bread(ROOTDEV, sb.logstart);
  memmove(buf->data, &log, sizeof(struct log_meta));
  bwrite(buf);
  brelse(buf);
}

void commit_tx() {
  struct log_meta log;
  struct buf *data_buf, *log_buf, *meta_buf;
  uint block_no;

  if (!holdingsleep(&loglock))
    panic("not holding lock");

  // Read log_meta in
  meta_buf = bread(ROOTDEV, sb.logstart);
  memmove(&log, meta_buf->data, sizeof(struct log_meta));

  // Set log to committed and write to disk
  log.commited = 1;
  memmove(meta_buf->data, &log, sizeof(struct log_meta));
  bwrite(meta_buf);

  // Write log blocks to their corresponding place on disk
  for (int i = 0; i < log.nchanges; i++) {
    block_no = log.blocknos[i];
    // cprintf("read bno at %d: %d\n", i, block_no);
    data_buf = bread(ROOTDEV, block_no);
    log_buf = bread(ROOTDEV, sb.logstart + 1 + i);

    memmove(data_buf->data, log_buf->data, sizeof(struct log_meta));
    bwrite(data_buf);
    brelse(data_buf);
    brelse(log_buf);

    data_buf->flags &= ~B_DIRTY;
  }

  // Set log to uncommitted and nchanges = 0, then write to disk
  memset(&log, 0, sizeof(struct log_meta));
  memmove(meta_buf->data, &log, sizeof(struct log_meta));
  bwrite(meta_buf);
  brelse(meta_buf);

  // Release log lock
  releasesleep(&loglock);
}

void log_write(struct buf *b) {
  struct log_meta log;
  struct buf *log_buf;
  struct buf *data_buf;

  if (!holdingsleep(&loglock))
    panic("not holding lock");

  b->flags |= B_DIRTY;

  // Read in log_meta
  log_buf = bread(ROOTDEV, sb.logstart);
  memmove(&log, log_buf->data, sizeof(struct log_meta));

  // Update blockno in log_meta and increment number of changes
  log.blocknos[log.nchanges] = b->blockno;
  log.nchanges++;

  data_buf = bread(ROOTDEV, sb.logstart + log.nchanges);
  memmove(data_buf->data, b->data, BSIZE);
  bwrite(data_buf);
  brelse(data_buf);

  // Write log_meta back to disk
  memmove(log_buf->data, &log, sizeof(struct log_meta));
  bwrite(log_buf);
  brelse(log_buf);
}
