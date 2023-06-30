#include <cdefs.h>
#include <defs.h>
#include <elf.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <trap.h>
#include <x86_64.h>

int exec(int argc, char *path, char **argv) {
  // your code here
  // Set up the virtual space to be stored in myproc
  struct vspace temp;
  if (vspaceinit(&temp) < 0) {
    vspacefree(&temp);
    return -1;
  }
  uint64_t rip;
  if (vspaceloadcode(&temp, path, &rip) < 0) {
    vspacefree(&temp);
    return -1;
  }
  if (vspaceinitstack(&temp, SZ_2G) < 0) {
    vspacefree(&temp);
    return -1;
  }

  // Set up user stack
  uint64_t args_addr[argc + 3];
  // Return PC (can be any value)
  args_addr[0] = 0;
  args_addr[argc + 1] = NULL;
  args_addr[argc + 2] = 0;

  uint64_t va = SZ_2G;
  for (int i = argc - 1; i >= 0; i--) {
    uint64_t size = strlen(argv[i]) + 1;
    va -= size;
    // Stack alignment
    if (size % 8 != 0)
      va -= (8 - size % 8);
    // Write values of arg[i] on stack
    if (vspacewritetova(&temp, va, argv[i], size) < 0) {
      vspacefree(&temp);
      return -1;
    }
    // Store address of argv[i]
    args_addr[i + 1] = va;
  }
  // Write arg[i] addresses on stack
  va -= (argc + 3) * 8;
  if (vspacewritetova(&temp, va, (char*) args_addr, (argc + 3) * 8) < 0) {
    vspacefree(&temp);
    return -1;
  }

  // Set up registers
  struct proc *p = myproc();
  p->tf->rip = rip;
  p->tf->rdi = argc;
  p->tf->rsp = va;
  p->tf->rsi = va + 8;

  // Copy the virtual space
  struct vspace oldvs = p->vspace;
  p->vspace = temp;
  vspaceinstall(p);
  vspacefree(&oldvs);

  return 0;
}
