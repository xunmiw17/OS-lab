# Lab 3 Design Doc: Address Space Management

## Overview
The goal of this assignment is to manage address space in xk, including implementing the sbrk() function which increases/decreases the heap size, growing user stack on demand, and achieving copy-on-write fork, which would reduce the overhead of copying the whole physical frames in fork().

### Major parts

sbrk:
Calling sbrk(n) would increase the heap size by n bytes. We need to allocate physical memory for the newly grown heap and update the bookkeeping structure.
Returns -1 if it can’t allocate enough space. Otherwise, return the previous heap limit (the old top of the heap)

Starting shell:
Follow the steps described in the spec.

Grow user stack on demand:
When a process starts, set up the user stack with an initial page. Grow the stack whenever the application issues an instruction that reads or writes to the user stack.

Copy-on-write fork:
Reduce the cost of fork by allowing multiple processes to share the same physical memory, while at a logical level still behaving as if the memory was copied.
As long as neither process modifies the memory, it can stay shared; if either process changes a page, a copy of that page will be made at that point (that is, copy-on-write).

Book keeping:
We need to add two field to core_map_entry
- reference count (how many virtual pages are referencing this physical frame)
- a lock to protect update on reference count

Besides, we need to add a field to vpage_info
- whether the page is a copy-on-write page


## In-depth Analysis and Implementation

### The functions you have to implement
#### sbrk `kernel/sysproc.c: sys_sbrk()`:
- If n <= 0, do nothing.
- If the newly allocated memory would touch the stack, return -1, meaning there isn’t enough memory to allocate.
- Allocates physical memory and updates its bookkeeping structure for user memory by calling the function vregionaddmap (kernel/vspace.c), and updates the heap size metadata.
- Call vspaceinvalidate (kernel/vspace.c) and vspaceinstall (kernel/vspace.c) to update the page table entries and installs them on the CPU.

#### `vspacecowcopy (kernel/vspace.c)`:
- For each region, recursively copy the vpage_info structure. Set the ppn of dst to that of src, increment the reference count of the physical frame, and set the page to be read only (set writable field to be 0).

### Existing functions you need to modify
- trap (`kernel/trap.c: trap()`)
In the “if” branch that checks if the trap is caused by page fault, check the error number to see if the page fault is caused by a not present page (last bit is 0). If it is, check the faulting virtual address is within 10 pages starting from the base of the stack. If it is, grow the stack by calling vregionaddmap (kernel/vspace.c) and update the stack size metadata. Call vspaceinvalidate (kernel/vspace.c) and vspaceinstall (kernel/vspace.c) to update the page table entries and install them on the CPU.
Also add a check to see if the trap is caused by a write to a COW page (the last three bits are "011" or "111"). If it is, decrement the reference count of the physical frame and allocates a new frame for the page by using kalloc. At last call vspaceinvalidate (kernel/vspace.c) and vspaceinstall (kernel/vspace.c) to update the page table entries and installs them on the CPU.
- kalloc (`kernel/kalloc.c: kalloc()`)
When an available physical frame is found, we increment the reference count
- kfree (`kernel/kalloc.c: kfree()`)
First check whether the reference count is 0 or 1. If it is then free the page, otherwise panic.
- fork (`kernel/proc.c`)
Replace vspacecopy with our implementation of vspacecowcopy

## Risk Analysis
### Unanswered question:
How would we check if a page is a COW page? In what conditions is a page COW?

### Time estimation:
- Best case: 10 h
- Worst case: 20 h
- Average: 15 h

### Staging of work:
We first implement `sbrk()`. Then modify trap.c to allow growing user stack on demand. At last we implement COW fork, including implementing `vspacecowcopy()`, adding field to core_map_entry and vpage_info, and modifying `kalloc()` and `kfree()`.
