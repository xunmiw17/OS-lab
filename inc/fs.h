#pragma once

#include "extent.h"
#include "param.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define INODEFILEINO 0 // inode file inum
#define ROOTINO 1      // root i-number
#define BSIZE 512      // block size
#define NEXTENT 30     // maximum number of extents of a file
#define NBLOCKS_PER_EXTENT 8  // number of blocks in one extent

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
  uint logstart;   // Block number of the start of log region
  uint inodestart; // Block number of the start of inode file
};

// On-disk inode structure
struct dinode {
  short type;         // File type (device, directory, regular file)
  short devid;        // Device number (T_DEV only)
  uint size;          // Size of file (bytes)
  struct extent data[NEXTENT]; // Data blocks of file on disk
  char pad[8];       // So disk inodes fit contiguosly in a block
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

// Log region header
struct log_header {
  int committed;
  int nblocks;
  uint blocknos[LOGSIZE - 1];
};

// Bitmap lock
extern struct sleeplock bmlk;
// Lock for the log region
extern struct sleeplock loglk;
