#include <cdefs.h>
#include <defs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>

// Interrupt descriptor table (shared by all CPUs).
struct gate_desc idt[256];
extern void *vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

int num_page_faults = 0;

void tvinit(void) {
  int i;

  for (i = 0; i < 256; i++)
    set_gate_desc(&idt[i], 0, SEG_KCODE << 3, vectors[i], KERNEL_PL);
  set_gate_desc(&idt[TRAP_SYSCALL], 1, SEG_KCODE << 3, vectors[TRAP_SYSCALL],
                USER_PL);

  initlock(&tickslock, "time");
}

void idtinit(void) { lidt((void *)idt, sizeof(idt)); }

void trap(struct trap_frame *tf) {
  uint64_t addr;

  if (tf->trapno == TRAP_SYSCALL) {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno) {
  case TRAP_IRQ0 + IRQ_TIMER:
    if (cpunum() == 0) {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case TRAP_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case TRAP_IRQ0 + 7:
  case TRAP_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n", cpunum(), tf->cs, tf->rip);
    lapiceoi();
    break;

  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;

      uint64_t err = tf->err;

      // Check if the page is a copy-on-write page
      struct vregion* vr = va2vregion(&myproc()->vspace, addr);
      if (vr != 0) {
        struct vpage_info* vpi = va2vpage_info(vr, addr);
        if (vpi != 0) {
          if ((err & 0x7) == 3 || (err & 0x7) == 7) {
            // Previous version
            if (vpi->is_cow) {
              struct core_map_entry *r = (struct core_map_entry *)
                                            pa2page(vpi->ppn << PT_SHIFT);
              acquire_core_map_lock();
              if (r->refer_count == 1) {
                release_core_map_lock();
              } else {
                r->refer_count--;
                release_core_map_lock();

                // Allocates a new physical frame and let the vpage_info point to it
                char* data;
                if (!(data = kalloc()))
                    exit();
                memmove(data, P2V(vpi->ppn << PT_SHIFT), PGSIZE);
                vpi->ppn = PGNUM(V2P(data));
              }

              vpi->is_cow = 0;
              vpi->writable = VPI_WRITABLE;

              // Flush TLB
              vspaceinvalidate(&myproc()->vspace);
              vspaceinstall(myproc());
              return;
            }
          }
        }
      }

      if ((err & 0x1) == 0) {
        // Does not let the stack exceed 10 pages
        if (addr <= SZ_2G && addr >= SZ_2G - PGSIZE * 10) {
          if (growstack() != -1)
            return;
        }
      }

      if (myproc() == 0 || (tf->cs & 3) == 0) {
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d rip %lx (cr2=0x%x)\n",
                tf->trapno, cpunum(), tf->rip, addr);
        panic("trap");
      }
    }

    // Assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "rip 0x%lx addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno, tf->err, cpunum(),
            tf->rip, addr);
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == TRAP_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

// Grow user stack by 1 page
int growstack() {
  struct vregion *stack_vr = &myproc()->vspace.regions[VR_USTACK];
  if (vregionaddmap(stack_vr, stack_vr->va_base - stack_vr->size - PGSIZE,
                    PGSIZE, VPI_PRESENT, VPI_WRITABLE) < 0)
      return -1;
  stack_vr->size += PGSIZE;

  vspaceinvalidate(&myproc()->vspace);
  vspaceinstall(myproc());
  return 0;
}
