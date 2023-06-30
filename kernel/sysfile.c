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

void spinlockinit(void) { initlock(&lk, "global_arr_lock"); }

int sys_dup(void) {
  // LAB1
  int fd;

  if (argfd(0, &fd) < 0)
    return -1;

  return dup(fd);
}

int sys_read(void) {
  // LAB1
  int fd;
  char* buf;
  int num;

  if (argfd(0, &fd) < 0 || argstr(1, &buf) < 0 || argint(2, &num) < 0)
    return -1;

  return read(fd, buf, num);
}

int sys_write(void) {
  // you have to change the code in this function.
  // Currently it supports printing one character to the screen.

  int fd;
  char *p;
  int n;

  if (argfd(0, &fd) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;

  return write(fd, p, n);
}

int sys_close(void) {
  // LAB1
  int fd;
  if (argfd(0, &fd) < 0)
    return -1;

  return close(fd);
}

int sys_fstat(void) {
  // LAB1
  int fd;
  struct stat *stat;

  if (argfd(0, &fd) < 0 || argptr(1, (char **)&stat, sizeof(stat)) < 0)
    return -1;

  return fstat(fd, stat);
}

int sys_open(void) {
  // LAB1
  char* file_path;
  int mode;

  if (argstr(0, &file_path) < 0 || argint(1, &mode) < 0)
    return -1;

  return open(file_path, mode);
}

int sys_exec(void) {
  char* path;
  char* argv[MAXARG];
  int addr;

  if (argstr(0, &path) < 0 || argint(1, &addr) < 0)
    return -1;

  for (int i = 0; i < MAXARG; i++) {
    int temp;
    if (fetchint(addr + sizeof(char*) * i, &temp) < 0) {
      return -1;
    }
    if (temp == 0) {
      return exec(i, path, argv);
    }
    if (fetchstr(temp, &argv[i]) < 0) {
      return -1;
    }
  }

  return -1;
}

int sys_pipe(void) {
  // LAB2
  int* arr;

  if (argptr(0, (char**) &arr, sizeof(int) * 2) < 0)
    return -1;

  return pipe(arr);
}

int sys_unlink(void) {
  // LAB 4
  char* file_path;

  if (argstr(0, &file_path) < 0)
    return -1;

  return unlink(file_path);
}
