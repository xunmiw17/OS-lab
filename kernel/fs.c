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

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

struct sleeplock bmlk;
void bmlkinit(void) { initsleeplock(&bmlk, "bitmap lock"); }

struct sleeplock loglk;
void loglkinit(void) { initsleeplock(&loglk, "log lock"); }

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// mark [start, end] bit in bp->data to 1 if used is true, else 0
static void bmark(struct buf *bp, uint start, uint end, bool used)
{
  int m, bi;
  for (bi = start; bi <= end; bi++) {
    m = 1 << (bi % 8);
    if (used) {
      bp->data[bi/8] |= m;  // Mark block in use.
    } else {
      if((bp->data[bi/8] & m) == 0)
        panic("freeing free block");
      bp->data[bi/8] &= ~m; // Mark block as free.
    }
  }
  bp->flags |= B_DIRTY; // mark our update
  log_write(bp);
}

// Blocks.

// Allocate n disk blocks, no promise on content of allocated disk blocks
// Returns the beginning block number of a consecutive chunk of n blocks
static uint balloc(uint dev, uint n)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for (b = 0; b < sb.size; b += BPB) {
    bp = bread(dev, BBLOCK(b, sb)); // look through each bitmap sector

    uint sz = 0;
    uint i = 0;
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      if ((bp->data[bi/8] & m) == 0) {  // Is block free?
        sz++;
        if (sz == 1) // reset starting blk
          i = bi;
        if (sz == n) { // found n blks
          bmark(bp, i, bi, true); // mark data block as used
          brelse(bp);
          return b+i;
        }
      } else { // reset search
        sz = 0;
        i =0;
      }
    }
    brelse(bp);
  }
  panic("balloc: can't allocate contiguous blocks");
}

// Free n disk blocks starting from b
static void bfree(int dev, uint b, uint n)
{
  struct buf *bp;

  assertm(n >= 1, "freeing less than 1 block");
  assertm(BBLOCK(b, sb) == BBLOCK(b+n-1, sb), "returned blocks live in different bitmap sectors");

  bp = bread(dev, BBLOCK(b, sb));
  bmark(bp, b % BPB, (b+n-1) % BPB, false);
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
// inum of 0 and starts at sb.startinode.
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
  memmove(icache.inodefile.data, di.data, sizeof(di.data));

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.inodestart);

  acquiresleep(&loglk);
  log_recover();
  releasesleep(&loglk);

  init_inodefile(dev);
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

// Writes the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
static void write_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  writei(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

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
    for (int i = 0; i < NEXTENT; i++) {
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

// Create an inode given a file path
struct inode *createi(char* file_path) {
  struct dinode dinode;
  dinode.type = T_FILE;
  dinode.devid = 1;
  dinode.size = 0;
  memset(dinode.data, 0, sizeof(dinode.data));

  // Append to inode file
  locki(&icache.inodefile);
  int nnode = icache.inodefile.size / sizeof(struct dinode);
  int inum = -1;
  struct dinode dip;
  for (int i = 0; i < nnode; i++) {
    read_dinode(i, &dip);
    if (dip.type == -1) {
      inum = i;
      break;
    }
  }
  if (inum == -1)
    inum = nnode;
  write_dinode(inum, &dinode);
  unlocki(&icache.inodefile);

  // Update root directory
  char name[DIRSIZ];
  struct inode *rootdir = nameiparent(file_path, name);

  struct dirent dirent;
  dirent.inum = inum;
  strncpy(dirent.name, name, DIRSIZ);
  locki(rootdir);
  int ndirent = rootdir->size / sizeof(struct dirent);
  int idx = -1;
  struct dirent de;
  for (int i = 0; i < ndirent; i++) {
    readi(rootdir, (char *) &de, i * sizeof(struct dirent), sizeof(struct dirent));
    if (de.name[0] == NULL) {
      idx = i;
      break;
    }
  }
  if (idx == -1)
    idx = ndirent;
  writei(rootdir, (char *) &dirent, idx * sizeof(struct dirent), sizeof(struct dirent));
  unlocki(rootdir);

  // Get the in-memory copy of the new inode and return it. If the inode is not
  // in memory, populate the empty inode with the corresponding dinode from disk
  struct inode *ip = iget(ROOTDEV, inum);
  read_dinode(ip->inum, &dip);

  return ip;
}

// Removes the file from the file system
int unlinki(char* file_path) {
  // Delete root directory entry
  char name[DIRSIZ];
  struct inode *rootdir = nameiparent(file_path, name);

  locki(rootdir);
  int ndirent = rootdir->size / sizeof(struct dirent);
  int inum = -1;
  struct dirent de;
  for (int i = 0; i < ndirent; i++) {
    readi(rootdir, (char *) &de, i * sizeof(struct dirent), sizeof(struct dirent));
    if (strncmp(de.name, name, DIRSIZ) == 0) {
      inum = i;
      de.name[0] = NULL;
      writei(rootdir, (char *) &de, i * sizeof(struct dirent), sizeof(struct dirent));
      break;
    }
  }
  unlocki(rootdir);

  // Remove the corresponding dinode in the inode file
  locki(&icache.inodefile);
  struct dinode dip;
  read_dinode(inum, &dip);
  // Free any extents the file was using
  for (int i = 0; i < NEXTENT; i++) {
    if (dip.data[i].nblocks == 0) {
      break;
    }
    bfree(ROOTDEV, dip.data[i].startblkno, dip.data[i].nblocks);
  }
  // Set the dinode's type to be -1 and write it as a placeholder to disk
  dip.type = -1;
  dip.size = 0;
  memset(dip.data, 0, sizeof(dip.data));
  write_dinode(inum, &dip);
  unlocki(&icache.inodefile);

  return 0;
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
  uint tot, m;
  struct buf *bp;

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

  // Recompute the offset to be that within the extent
  int sz = 0;
  int cnt = 0;
  while (sz + ip->data[cnt].nblocks * BSIZE < off) {
    sz += ip->data[cnt].nblocks * BSIZE;
    cnt++;
  }
  off -= sz;

  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    if (off == ip->data[cnt].nblocks * BSIZE) {
      cnt++;
      m = 0;
      off = 0;
    } else {
      bp = bread(ip->dev, ip->data[cnt].startblkno + off / BSIZE);
      m = min(n - tot, BSIZE - off % BSIZE);
      memmove(dst, bp->data + off % BSIZE, m);
      brelse(bp);
    }
  }
  return n;
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

int writei(struct inode *ip, char *src, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }

  if (off > ip->size || off + n < off)
    return -1;

  // Recompute the offset to be that within the extent
  int sz = 0;
  int cnt = 0;
  while (sz + ip->data[cnt].nblocks * BSIZE < off) {
    sz += ip->data[cnt].nblocks * BSIZE;
    cnt++;
  }
  uint off_extent = off - sz;

  for (tot = 0; tot < n; tot += m, off_extent += m, src += m) {
    if (cnt >= NEXTENT) {
      n = tot;
      break;
    }
    // Need to allocate more extents
    if (ip->data[cnt].startblkno == 0) {
      acquiresleep(&bmlk);
      ip->data[cnt].startblkno = balloc(ROOTDEV, NBLOCKS_PER_EXTENT);
      releasesleep(&bmlk);

      ip->data[cnt].nblocks = NBLOCKS_PER_EXTENT;
    }
    if (off_extent == ip->data[cnt].nblocks * BSIZE) {
      cnt++;
      m = 0;
      off_extent = 0;
    } else {
      bp = bread(ip->dev, ip->data[cnt].startblkno + off_extent / BSIZE);
      m = min(n - tot, BSIZE - off_extent % BSIZE);
      memmove(bp->data + off_extent % BSIZE, src, m);
      log_write(bp);
      brelse(bp);
    }
  }

  int extra = off + n - ip->size;
  if (extra > 0) {
    ip->size += extra;

    // Update the dinode on disk
    struct dinode dip;
    read_dinode(ip->inum, &dip);
    dip.size = ip->size;
    memmove(dip.data, ip->data, sizeof(ip->data));

    write_dinode(ip->inum, &dip);
  }

  return n;
}

// Begin transaction
void log_begin_tx() {
  acquiresleep(&loglk);

  struct buf* header_buf = bread(ROOTDEV, sb.logstart);
  struct log_header* header = (struct log_header*) header_buf->data;
  header->committed = 0;
  header->nblocks = 0;
  bwrite(header_buf);
  brelse(header_buf);
}

// Write the data in the given buffer to the log region
void log_write(struct buf* b) {
  // Get the log header metadata
  struct buf* header_buf = bread(ROOTDEV, sb.logstart);
  struct log_header* header = (struct log_header*) header_buf->data;
  int nblocks = header->nblocks;
  int blockno_to_write = sb.logstart + 1 + nblocks;

  // Write the block data to log
  struct buf* data_buf = bread(ROOTDEV, blockno_to_write);
  memmove(data_buf->data, b->data, BSIZE);
  bwrite(data_buf);
  brelse(data_buf);

  // Update the log header metadata
  header->blocknos[nblocks] = b->blockno;
  header->nblocks++;
  bwrite(header_buf);
  brelse(header_buf);
}

// Commit the log: move the log data back to their actual locations
void log_commit_tx() {
  // Set the commit flag to 1
  struct buf* header_buf = bread(ROOTDEV, sb.logstart);
  struct log_header* header = (struct log_header*) header_buf->data;
  int nblocks = header->nblocks;
  header->committed = 1;
  bwrite(header_buf);
  brelse(header_buf);

  // Copy every data blocks in the log region to their actual locations on disk
  for (int i = 0; i < nblocks; i++) {
    // Get the block number
    uint blockno = header->blocknos[i];

    // Get the block data
    struct buf* data_buf = bread(ROOTDEV, sb.logstart + 1 + i);

    // Write to the actual block location
    struct buf* actual_block_buf = bread(ROOTDEV, blockno);
    memmove(actual_block_buf->data, data_buf->data, BSIZE);
    bwrite(actual_block_buf);
    brelse(actual_block_buf);
    brelse(data_buf);
  }

  // Reset the commit flag to 0
  header_buf = bread(ROOTDEV, sb.logstart);
  header = (struct log_header*) header_buf->data;
  header->committed = 0;
  bwrite(header_buf);
  brelse(header_buf);

  releasesleep(&loglk);
}

// Recover the log if the commit flag is set
void log_recover() {
  struct buf* header_buf = bread(ROOTDEV, sb.logstart);
  struct log_header* header = (struct log_header*) header_buf->data;
  int nblocks = header->nblocks;
  if (header->committed == 1) {
    // Copy every data blocks in the log region to their actual locations on disk
    for (int i = 0; i < nblocks; i++) {
      // Get the block number
      uint blockno = header->blocknos[i];

      // Get the block data
      struct buf* data_buf = bread(ROOTDEV, sb.logstart + 1 + i);

      // Write to the actual block location
      struct buf* actual_block_buf = bread(ROOTDEV, blockno);
      memmove(actual_block_buf->data, data_buf->data, BSIZE);
      bwrite(actual_block_buf);
      brelse(actual_block_buf);
      brelse(data_buf);
    }

    // Reset the commit flag to 0
    header->committed = 0;
    bwrite(header_buf);
  }
  brelse(header_buf);
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

// Look up and return the inode for a path name. Returns NULL if not found
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

/*
See namex
*/
struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

/*
See namex
*/
struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}

