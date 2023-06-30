#pragma once

#include <extent.h>
#include <sleeplock.h>
#include <mmu.h>
#include <fs.h>

#define PIPELEN (PGSIZE - sizeof(char*) - 5 * sizeof(int) - sizeof(struct spinlock))

// in-memory copy of an inode
struct inode {
  uint dev;  // Device number
  uint inum; // Inode number
  int ref;   // Reference count
  int valid; // Flag for if node is valid
  struct sleeplock lock;

  // copy of disk inode (see fs.h for details)
  short type;
  short devid;
  uint size;
  struct extent data[NEXTENT];
};

// table mapping device ID (devid) to device functions
struct devsw {
  int (*read)(struct inode *, char *, int);
  int (*write)(struct inode *, char *, int);
};

struct file_info {
  struct inode* inode;
  int offset;
  int mode;
  int refer_count;

  struct pipe_info* pipe_info;
};

struct pipe_info {
  char pipe[PIPELEN];
  int read_offset;
  int write_offset;
  int read_end_open;  // ref count
  int write_end_open; // ref count
  int num_avail;      // number of empty bytes in pipe
  struct spinlock lock;
};

extern struct file_info global_arr[];
extern struct spinlock lk;

extern struct devsw devsw[];

// Device ids
enum {
  CONSOLE = 1,
};
