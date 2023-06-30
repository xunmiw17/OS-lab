#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <memlayout.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <spinlock.h>
#include <trap.h>
#include <x86_64.h>
#include <fs.h>
#include <file.h>
#include <vspace.h>
#include <fcntl.h>

// process table
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

// to test crash safety in lab5,
// we trigger restarts in the middle of file operations
void reboot(void) {
  uint8_t good = 0x02;
  while (good & 0x02)
    good = inb(0x64);
  outb(0x64, 0xFE);
loop:
  asm volatile("hlt");
  goto loop;
}

void pinit(void) { initlock(&ptable.lock, "ptable"); }

// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc *allocproc(void) {
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->killed = 0;

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0) {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trap_frame *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 8;
  *(uint64_t *)sp = (uint64_t)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->rip = (uint64_t)forkret;

  return p;
}

// Set up first user process.
void userinit(void) {
  struct proc *p;
  extern char _binary_out_initcode_start[], _binary_out_initcode_size[];

  p = allocproc();

  initproc = p;
  assertm(vspaceinit(&p->vspace) == 0, "error initializing process's virtual address descriptor");
  vspaceinitcode(&p->vspace, _binary_out_initcode_start, (int64_t)_binary_out_initcode_size);
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ss = (SEG_UDATA << 3) | DPL_USER;
  p->tf->rflags = FLAGS_IF;
  p->tf->rip = VRBOT(&p->vspace.regions[VR_CODE]);  // beginning of initcode.S
  p->tf->rsp = VRTOP(&p->vspace.regions[VR_USTACK]);

  safestrcpy(p->name, "initcode", sizeof(p->name));

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void) {
  // your code here
  struct proc* proc = myproc();

  // Create an entry in the process table
  struct proc *child_p = allocproc();
  if (child_p == 0)
    return -1;

  assertm(vspaceinit(&child_p->vspace) == 0, "error initializing process's virtual address descriptor");

  // Copy virtual memory space
  vspacecowcopy(&child_p->vspace, &proc->vspace);

  // Copy trap frame
  memmove(child_p->tf, proc->tf, sizeof(*proc->tf));

  // Copy files
  acquire(&ptable.lock);
  for (int i = 0; i < NOFILE; i++) {
    if (proc->file_info[i] != NULL) {
      proc->file_info[i]->refer_count += 1;
      child_p->file_info[i] = proc->file_info[i];
      // If it is a pipe, increase the reader/writer count
      if (proc->file_info[i]->inode == 0) {
        acquire(&proc->file_info[i]->pipe_info->lock);
        if (proc->file_info[i]->mode == O_RDONLY) {
          proc->file_info[i]->pipe_info->read_end_open++;
        } else {
          proc->file_info[i]->pipe_info->write_end_open++;
        }
        release(&proc->file_info[i]->pipe_info->lock);
      }
    }
  }
  child_p->state = RUNNABLE;
  child_p->tf->rax = 0;
  child_p->parent = proc;
  release(&ptable.lock);

  return child_p->pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void) {
  struct proc* proc = myproc();
  // Close all open files
  for (int i = 0; i < NOFILE; i++) {
    if (proc->file_info[i] != NULL) {
      close(i);
    }
  }
  // If the process has children, let the initial process reclaim the children's
  // data
  acquire(&ptable.lock);
  for (int i = 0; i < NPROC; i++) {
    if (ptable.proc[i].parent == proc) {
      ptable.proc[i].parent = initproc;
    }
  }
  proc->state = ZOMBIE;
  wakeup1(myproc()->parent);
  sched();
  release(&ptable.lock);
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void) {
  // your code here
  // Scan through table looking for exited children.
  int found = 0;

  acquire(&ptable.lock);
  for (int i = 0; i < NPROC; i++) {
    if (ptable.proc[i].parent == myproc()) {
      found = 1;
      break;
    }
  }
  release(&ptable.lock);

  // this process has no children
  if (!found) {
    return -1;
  }

  acquire(&ptable.lock);
  while (1) {
    for (int i = 0; i < NPROC; i++) {
      // Suspend execution if a child terminates which calls exit (ZOMBIE) or is killed
      if (ptable.proc[i].parent == myproc() && (ptable.proc[i].state == ZOMBIE)) {
        int child_pid = ptable.proc[i].pid;
        ptable.proc[i].state = UNUSED;
        ptable.proc[i].pid = 0;
        ptable.proc[i].parent = NULL;
        ptable.proc[i].killed = 0;
        // Free the process's stack, heap, code, and page tables
        vspacefree(&ptable.proc[i].vspace);
        // Free the child's kernel stack
        kfree(ptable.proc[i].kstack);
        release(&ptable.lock);
        return child_pid;
      }
    }
    sleep(myproc(), &ptable.lock);
  }
  release(&ptable.lock);

  return -1;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void scheduler(void) {
  struct proc *p;

  for (;;) {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
      if (p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      mycpu()->proc = p;
      vspaceinstall(p);
      p->state = RUNNING;
      swtch(&mycpu()->scheduler, p->context);
      vspaceinstallkern();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      mycpu()->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void) {
  int intena;

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1) {
    cprintf("pid : %d\n", myproc()->pid);
    cprintf("ncli : %d\n", mycpu()->ncli);
    cprintf("intena : %d\n", mycpu()->intena);

    panic("sched locks");
  }
  if (myproc()->state == RUNNING)
    panic("sched running");
  if (readeflags() & FLAGS_IF)
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&myproc()->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void) {
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void) {
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk) {
  if (myproc() == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock) { // DOC: sleeplock0
    acquire(&ptable.lock);  // DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  myproc()->chan = chan;
  myproc()->state = SLEEPING;
  sched();

  // Tidy up.
  myproc()->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock) { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void wakeup1(void *chan) {
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan) {
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid) {
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void) {
  static char *states[] = {[UNUSED] = "unused",   [EMBRYO] = "embryo",
                           [SLEEPING] = "sleep ", [RUNNABLE] = "runble",
                           [RUNNING] = "run   ",  [ZOMBIE] = "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint64_t pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    if (p->state != 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING) {
      getcallerpcs((uint64_t *)p->context->rbp, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

struct proc *findproc(int pid) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid)
      return p;
  }
  return 0;
}
