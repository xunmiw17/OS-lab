#include <cdefs.h>
#include <date.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <x86_64.h>

int sys_crashn(void) {
  int n;
  if (argint(0, &n) < 0)
    return -1;

  crashn_enable = 1;
  crashn = n;

  return 0;
}

int sys_fork(void) { return fork(); }

void halt(void) {
  while (1)
    ;
}

void sys_exit(void) {
  // LAB2
  exit();
}

int sys_wait(void) { return wait(); }

int sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int sys_getpid(void) { return myproc()->pid; }

/*
 * arg0: integer value of amount of memory to be added to the heap. If arg0 < 0, treat it as 0.
 *
 * Adds arg0 to the current heap.
 * Returns the previous heap limit address, or -1 on error.
 *
 * Error condition:
 * Insufficient space to allocate the heap.  Note that if some space
 * exists but that space is insufficient to handle the complete request,
 * -1 should still be returned, and nothing should be added to the heap.
 */
int sys_sbrk(void) {
  // LAB3
  int n;

  if (argint(0, &n) < 0)
      return -1;
  struct vregion *vr = &myproc()->vspace.regions[VR_HEAP];
  int prev_limit = vr->va_base + vr->size;

  if (n == 0)
    return prev_limit;
  if (n < 0) {
    // Delete physical memory
    n = -n;
    if (n > vr->size)
      return prev_limit;
    if (vregiondelmap(vr, prev_limit - n, n) < 0)
      return -1;
    vr->size -= n;
  } else {
    // Cannot allocate enough space
    if (prev_limit + n > SZ_2G - myproc()->vspace.regions[VR_USTACK].size)
      return -1;
    // Allocates physical memory and maps the new n bytes in the heap to that physical memory
    if (vregionaddmap(vr, prev_limit, n, VPI_PRESENT, VPI_WRITABLE) < 0)
        return -1;
    vr->size += n;
  }
  // Rebuilds the process's page table
  vspaceinvalidate(&myproc()->vspace);
  // Installs the process's page table on the cpu
  vspaceinstall(myproc());
  return prev_limit;
}

int sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
