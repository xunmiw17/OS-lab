# Lab 3: Address Space Management
## Design Doc Due: 02/10/2023 (Friday) at 11:59pm. No late days.
## Complete Lab Due: 02/17/2023 (Friday) at 11:59pm.

To get lab3 handout and tests, run the command

```
git pull upstream main
```

and merge.

## Introduction

In this lab, we are going to cover address space management. With it, you will
be able to run xk's shell. We also ask you to implement some common techniques to save memory.

### Setting up the init process

At this point, you may have noticed that binary `initcode` contains the test code `user/lab2test.c`. `kernel/initcode.S` is the starting point of the user-level application. It calls `main` in `user/lab2test.c`. We now change `kernel/initcode.S` so that it calls `exec`. This allows the user to change the initialization code (`user/lab3init.c` and `user/init.c`) without recompiling the kernel.

To do this, look at `kernel/Makefrag`, you will find this section:
```
$(O)/initcode : kernel/initcode.S user/lab2test.c $(ULIB)
	$(CC) -nostdinc -I inc -c kernel/initcode.S -o $(O)/initcode.o
	$(CC) -ffreestanding -MD -MP -mno-sse -I inc -c user/lab2test.c -o $(O)/lab2test.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 1000 -o $(O)/initcode.out $(O)/initcode.o $(O)/lab2test.o $(ULIB)
	$(OBJCOPY) -S -O binary $(O)/initcode.out $(O)/initcode
	$(OBJDUMP) -S $(O)/initcode.out > $(O)/initcode.asm
```

Remove the dependency of `user/lab2test.c` in the initcode make rule by replacing the snippet above to the following:
```
$(O)/initcode : kernel/initcode.S
	$(CC) -nostdinc -I inc -c kernel/initcode.S -o $(O)/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 1000 -o $(O)/initcode.out $(O)/initcode.o
	$(OBJCOPY) -S -O binary $(O)/initcode.out $(O)/initcode
	$(OBJDUMP) -S $(O)/initcode.o > $(O)/initcode.asm
```

Also change the content of `kernel/initcode.S` to
```
#include <syscall.h>
#include <trap.h>

.globl start
start:
  mov $init, %rdi
  mov $argv, %rsi
  mov $SYS_exec, %rax
  int $TRAP_SYSCALL

exit:
  mov $SYS_exit, %rax
  int $TRAP_SYSCALL
  jmp exit

init:
  .string "/lab3init\0"

.p2align 2
argv:
  .quad init
  .quad 0
```

After you change the parts above, xk will start `kernel/initcode.S` as the first program. `kernel/initcode.S` will load `user/lab3init.c` on the disk, which will open the console and exec `user/lab3test.c`. Later on we will change this to load into a shell!

## Part 1: Create a user-level heap
A process that needs more memory at runtime can call `sbrk` (set program break) to grow its
heap size. The common use case is the situation where a user library routine,
`malloc` in C or `new` in C++, calls `sbrk` whenever the application
asks to allocate a data region that cannot fit on the current heap
(e.g., if the heap is completely allocated due to prior calls to `malloc`).

For example, if a user application wants to increase the
heap size by `n` bytes, it calls `sbrk(n)`. `sbrk(n)` returns the OLD limit.
In UNIX, the user application can also decrease
the amount of the heap by passing negative values to `sbrk`, but
you do not need to support this.  Generally, the user library asks `sbrk` to provide
more space than immediately needed, to reduce the number of system calls.

As a special case, when a user application is loaded via `exec`,
the user program is initialized to have a zero-size heap
(i.e., `vregion[VR_HEAP].size = 0`), and so the first call to `malloc`
always calls `sbrk`.

xk has to track how much memory has been allocated to each process's heap.
xk also has to allocate and free memory pages (using `kalloc` and `kfree`) on
behalf of the user. Remember, xk allocates and frees memory at page granularity
(i.e., 4096 bytes) but `sbrk` needs to support allocating/deallocating memory at
byte granularity. The OS does this to be portable, since an application cannot depend
on the machine adopting a specific page size.

The kernel is responsible for allocating physical memory for the newly grown heap and updating its 
bookkeeping structure for user memory (both can be achieved through `vregionaddmap` in `kernel/vspace.c`).
It also needs to update the actual page table entries to reflect the newly added mappings (through `vspaceinvalidate` in `kernel/vspace.c`).

In user space, we have provided an implementation of `malloc` and `free` (in `user/umalloc.c`) that is going to use `sbrk`. After the implementation of `sbrk` is
done, user-level applications should be able to call `malloc` and `free`.

### Exercise
Implement `sbrk`.

#### What to Implement
```c
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
int
sys_sbrk(void);
```

### Optional Exercise
Implement `sbrk` with decrement. Above, we said if `n < 0`, we treat `n` as 0. However,
`sbrk(n)` is used to deallocate memory in the kernel as well. The kernel would deallocate
`abs(n)` bytes of memory, and returns the previous heap limit. If the allocated memory is
less than `abs(n)` bytes (not enough memory to deallocate), it would behave the same as `sbrk(0)`.

If you decide to implement `sbrk` with decrement,
you will see `Good job! sbrk decrement implemented!` if you implement deallocating `abs(n)` bytes correctly,
you will see `sbrk decrement test passed!` if you handle edge cases correctly.

Hint: Implement a `vregiondelmap` in `kernel/vspace.c`. Be aware of signed/unsigned integer arithmetic.


### Question #1
Why might an application prefer using `malloc` and `free` instead of using `sbrk` directly? What is the relationship between `malloc`/`free` and `sbkr`?

## Part 2: Starting shell
A shell is a typical user interface for operating systems. A shell is already
implemented (in `user/sh.c`) for you and it is your task to load it after xk
boots. Previously in lab1 and lab2, the test code (`user/lab1test.c`, `user/lab2test.c`)
was directly linked to the kernel binary and thus did not need `exec` to run.
From this lab on, all the test code and other user applications need to be loaded
from disk.

In order to run the shell, change this line in `kernel/initcode.S`:
```
init:
  .string "/lab3init\0"
```
to
```
init:
  .string "/init\0"
```

After you change the parts above, xk will start with `kernel/initcode.S`, but
will load the binary compiled from `user/init.c` instead of `user/lab3init.c`. 
`user/init.c` will fork into two processes. One will load `user/sh.c`, the other
will wait for zombie processes to reap. After these changes, when you boot xk,
you should see the following:

```
xk...
CPU: QEMU Virtual CPU version 2.5+
  fpu de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2
  sse3 cx16 hypervisor
  syscall nx lm
  lahf_lm svm
E820: physical memory map [mem 0x9000-0x908f]
  [mem 0x0-0x9fbff] available
  [mem 0x9fc00-0x9ffff] reserved
  [mem 0xf0000-0xfffff] reserved
  [mem 0x100000-0xfdffff] available
  [mem 0xfe0000-0xffffff] reserved
  [mem 0xfffc0000-0xffffffff] reserved

cpu0: starting xk

free pages: 3354
cpu0: starting
sb: size 100000 nblocks 67205 bmap start ...
init: starting sh
$
```

At this point, you will execute your tests in the shell. In GDB, the `initcode` command will load the symbol table for `init`, not the currently running test code. To step through the test code, use, for example, `lab3test` to load symbols for `lab3test.c`. You can do something similar for lab 1 and 2 tests.

### Exercise
Allow xk to boot into the shell. Try a set of commands like `cat`, `echo`, `grep`,
`ls`, `wc` along with `|` (i.e. pipe) in the shell to see if it works properly.

### Question #2:
Explain how the file descriptors are set up by the xk's shell program in order to run the command `ls | wc`.

## Part 3: Grow user stack on-demand
In the rest of lab3, we study how to reduce xk's memory consumption. The first
technique is to grow the user stack on-demand. In your implementation of `exec`,
the user stack size is fixed and is allocated before the user application starts.
However, we can change that to allocate only the memory that is needed
at run-time. Whenever a user application issues an instruction that
reads or writes to the user stack (e.g., creating a stack frame, accessing local
variables), we grow the stack as needed.

When the user process starts, you should set up the user stack with
an initial page to store application arguments.

To implement on-demand stack growth, you will need to understand how to
handle page faults.
A page fault is a hardware exception that occurs when a program accesses
a virtual memory page without a valid page table entry, or with a valid
entry, but where the program does not have permission to perform the
operation.

On the hardware exception, control will trap into the kernel; the exception
handler should add memory to the stack region and resume execution in the user program.

The code for handling this can be seen in `trap.c`.
```c
  default:
    addr = rcr2();

    if (tf->trapno == TRAP_PF) {
      num_page_faults += 1;
```
`rcr2()` returns the address that was attempted to be accessed. <br />
More information on the page fault can be obtained by examining `tf->err`.
The last 3 bits indicate key information. <br />
b<sub>31</sub>...b<sub>2</sub>b<sub>1</sub>b<sub>0</sub><br />
b<sub>2</sub> is set if the fault occurred in usermode.<br />
b<sub>1</sub> is set if the fault occurred on a write.<br />
b<sub>0</sub> is set if it was a page protection issue. This is not set if the page is not present.<br />
More can be found [here](https://wiki.osdev.org/Exceptions#Page_Fault)

**In this lab, you can design your stack grower to never exceed 10 stack pages.**

### Question #3:
When a syscall completes, user-level execution resumes with the instruction
immediately after the syscall.  When a page fault exception completes, where does
user-level execution resume?

### Question #4:
How should the kernel decide whether an unmapped reference is a normal stack
operation versus a stray pointer dereference that
should cause the application to halt? What should happen, for example, if
an application calls a procedure with a local variable that is an array
of a million integers?  

### Exercise
Implement growing the user stack on-demand. Note that our test code
uses the system call `sysinfo` to figure out how much memory is used.

### Question #5:
Is it possible to reduce the user stack size at
run-time (i.e., to deallocate the user stack when a procedure with a
large number of local variables goes out of scope)?  If so, sketch how that
might work.

## Warning
At this point we will be editing fork. It is possible to get in a state where
no process will be able to start. We strongly suggest you checkpoint your code
in some way (using a git tag, or copying the code into a directory you won't touch).

## Part 4: Copy-on-write fork
The next optimization improves the performance of fork by
using a copy-on-write mechanism. Currently, `fork` duplicates every page
of user memory in the parent process.  Depending on the size
of the parent process, this can consume a lot of memory
and can potentially take a long time.  All this work is thrown away
if the child process immediately calls `exec`.

Here, we reduce the cost of `fork` by allowing multiple processes
to share the same physical memory, while at a logical level
still behaving as if the memory was copied.  As long as neither process
modifies the memory, it can stay shared; if either process changes
a page, a copy of that page will be made at that point
(that is, copy-on-write).

When `fork` returns, the child process is given a page table that
points to the same memory pages as the parent.  No additional memory
is allocated for the new process, other than to hold the new page table.
However, the page tables of both processes will need to
be changed to be read-only.  That way, if either process tries
to alter the content of their memory, a trap to the kernel will occur,
and the kernel can make a copy of the memory page at that point,
before resuming the user code. As a start, it will be helpful to understand
how `vspacecopy` works, and then implement your own `vspacecowcopy`.

Once you enable copy-on-write sharing, multiple processes may now map to the
same set of frames in the physical memory. You will need to track how many pages 
are mapped onto each frame in order to know when a frame can be actually freed. 
You will also want to make sure that your updates to the reference count is protected.
You can allocate a frame via `kalloc`, and each frame has an associated `core_map_entry` that
you can use to store information. You can modify `kalloc` and `kfree` to work with your newly added field.

For every page in a process's page table (vspace), there is a structure that
represents that entry (`struct vpage_info`). Feel free to add fields,
potentially to indicate this virtual page is a copy-on-write page. On a page
fault, you can use the vspace functions (`va2vregion` and `va2vpage_info`) to
get the `vpage_info` and understand what caused the fault.

On a page fault you can see if the virtual address is a copy-on-write address.
From here you can allocate a page, copy the data from the copy-on-write page,
and let the faulting process start writing to that freshly-allocated page.
Note that whenever you are changing the translation or changing the permission of 
a page, you need to flush the TLB to make sure you don't use the cached stale translations (see Q6).

A tricky part of the assignment is that, of course, a child process
with a set of copy-on-write pages can fork another child.
Thus, any physical memory page can be shared by many processes.
There are various possible ways of keeping track of which pages
are cached; we do not specify how. Instead, we will only test for
functional correctness -- that the number of allocated pages is
small and that the child processes execute as if they received a
complete copy of the parent's memory.

<img src="cow_flag.jpg" width="500"/>

### Tips
You will see a lot of addresses and bookkeeping structures in this lab:
- user virtual address: this is a virtual address within a process's virtual address space, it will be between [0, SZ_2G)
- vpage_info: each page in a process's virtual address space has a corresponding `vpage_info` struct, you can use it to track page specific info. 
To get the `vpage_info`, you need to first find the `vregion` of the page (`va2vregion`), and then you can use `va2vpage_info`.
- kernel virtual address: this is a virtual address within the kernel's virtual address sapce, it will be above KERNBASE. The kernel code, data, and 
  heap all use kernel virtual addresses (print out the address of any kernel code/data to see it for yourself!). When you call `kalloc`, a kernel 
  virtual address is returned upon success, the returned address is guaranteed to be backed by a physical frame. You can use this to allocate dynamic data in the kernel (kstack, pipe),
  or you can hand this to a user process by mapping a process's page to this frame. For addresses returned by `kalloc`, you can call `V2P` to get the physical address backing this page.
- core_map_entry: this is a bookkeeping structure for each physical frame. To retrieve this, you can call `pa2page` with the physical address.
- `P2V`: get a kernel virtual address from a physical address
- `V2P`: get a physical address from a kernel virtual address
- To get physical address, leftshift ppn of a vpage_info by `PT_SHIFT` (ex. vpage_info->ppn << PT_SHIFT)
- To get ppn from a core_map_entry, you can use `PGNUM(page2pa(page))` or `page - core_map`.
<b>The virtual address the core_map_entry stores does not have a 1 to 1 relationship with ppn's, and should not be used to get the ppn </b>.
- Some functions require physical address, some virtual. Be careful about this detail
- Be careful when checking for reference counts in `kfree`. On boot, the system calls `kfree` on each page BEFORE ever calling `kalloc`. This means it's possible for reference counts to be 0 in kfree. 


### Question #6:
The TLB caches the page table entries of recently referenced
pages.  When you modify the page table entry to allow write access,
which function should you use in xk ensure that the TLB does not have a stale version of the cache?
How does it work?

### Exercise
Implement copy-on-write fork.

## Testing and hand-in
After you implement the system calls described above. The kernel should be able
to print `lab3 tests passed!`. You should also use `sysinfo` in the shell to
detect potential memory leaks when running `lab3test`. If your implementation
is correct, `pages_in_use` should be kept the same before and after running `lab3test`.

Running the tests from the _previous_ labs is a good way to boost your confidence
in your solution for _this_ lab. If you want to run the lab1/lab2 tests,
comment out the lines that call `open` and `dup` on the console,
since `init` now does this before running the tests. 

### Question #7
For each member of the project team, how many hours did you
spend on this lab?

### Question #8
What did you like or dislike about this lab? Is there anything you wish you knew earlier?

Create a file `lab3.txt` in the top-level xk directory with
your answers to the questions listed above.

When you're finished, create an `end_lab3` git tag as described above,
so we know the point at which you submitted your code.

### lab3test expected output
lab3test output should be close to the following: 
```
memtest
mem ok
memtest passed
sbrktest
pids 4-43 (6-45 if ran after sh) should be killed with trap 14 err 5
pid 6 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80000000--kill proc
pid 7 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8000c350--kill proc
pid 8 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800186a0--kill proc
pid 9 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800249f0--kill proc
pid 10 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80030d40--kill proc
pid 11 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8003d090--kill proc
pid 12 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800493e0--kill proc
pid 13 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80055730--kill proc
pid 14 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80061a80--kill proc
pid 15 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8006ddd0--kill proc
pid 16 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8007a120--kill proc
pid 17 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80086470--kill proc
pid 18 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800927c0--kill proc
pid 19 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8009eb10--kill proc
pid 20 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800aae60--kill proc
pid 21 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800b71b0--kill proc
pid 22 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800c3500--kill proc
pid 23 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800cf850--kill proc
pid 24 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800dbba0--kill proc
pid 25 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800e7ef0--kill proc
pid 26 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x800f4240--kill proc
pid 27 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80100590--kill proc
pid 28 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8010c8e0--kill proc
pid 29 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80118c30--kill proc
pid 30 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80124f80--kill proc
pid 31 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801312d0--kill proc
pid 32 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8013d620--kill proc
pid 33 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80149970--kill proc
pid 34 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80155cc0--kill proc
pid 35 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80162010--kill proc
pid 36 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8016e360--kill proc
pid 37 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8017a6b0--kill proc
pid 38 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80186a00--kill proc
pid 39 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x80192d50--kill proc
pid 40 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x8019f0a0--kill proc
pid 41 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801ab3f0--kill proc
pid 42 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801b7740--kill proc
pid 43 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801c3a90--kill proc
pid 44 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801cfde0--kill proc
pid 45 : trap 14 err 5 on cpu 0 rip 0x497 addr 0x801dc130--kill proc
free pages after out-of-memory(shouldn't be 0): 230
Good job! sbrk decrement implemented!
sbrk decrement test passed!
sbrktest passed
growstacktest
pages_in_use before stack allocation = 147
pages_in_swap before stack allocation = 0
successfully added another page
successfully added another page
successfully added another page
successfully added another page
successfully added another page
successfully added another page
successfully added another page
successfully added another page
pages_in_use after stack allocation = 155
pages_in_swap after stack allocation = 0
growstacktest passed
growstacktest_edgecase
next 2 processes should be killed with trap 14 err 6
pid 47 : trap 14 err 6 on cpu 0 rip 0xaf4 addr 0x7fff5000--kill proc
pid 48 : trap 14 err 6 on cpu 0 rip 0xbc1 addr 0x80000000--kill proc
growstacktest_edgecase passed
copyonwriteforktest
pages_in_use before copy-on-write fork = 355
pages_in_swap before copy-on-write fork = 0
pages_in_use after copy-on-write fork = 394
pages_in_swap after copy-on-write fork = 0
pages_in_use after read = 394
pages_in_swap after read = 0
pages_in_use after write = 594
pages_in_swap after write = 0
copyonwriteforktest passed
lab3 tests passed!!
```
