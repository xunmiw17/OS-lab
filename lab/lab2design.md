# Lab 2 Design Doc: Multiprocessing

## Overview
The goal of this assignment is to add multiprocessing to xk, which implements system calls to allow creating new processes and interprocess communication, including fork, wait, exit, pipe and exec.

### Major parts

Synchronization issues:
The original system only used one process to make system calls, so no need for synchronization for any global data used by syscalls. Since we want to add multiprocessing to xk, protecting global variables accessed and preventing interference of the sharing of resources are the synchronization goals.
Challenge: release locks correctly on error conditions.

fork:
Create a new process by duplicating the calling process.
Challenge: how to return twice.

wait:
Sleep until a child process terminates, then return that child’s PID.
need to reclaim child's kernel resources, including child's PCB, page tables, and kernel stack.
Process shouldn’t return from here until a child has exited.
Process shouldn't block if any child already exited.
Challenge: the use of condition variables to wait for a child to exit.

exit:
Halts program and sets state to have its resources reclaimed.
Should clean up as much resources as possible (eg. close all open files) and let your parent know you've exited.

pipe:
Creates a pipe (kernel buffer) for process to read and write, and returns two new file descriptors.
Challenge: process multiple read/write ends.

exec:
Fully replaces the current process, it does not create a new one.
Challenge: setup all the required kernel data structures for the new program.

How the different parts of the design interact together?
The `wait` system call interacts with `fork` and `exit`: it is called by the parent process to wait for a child.

Synchronization:
Use a spinlock to protect the global file table to allow one process accessing it at a time. Also, add a spinlock to each file_info struct to maximize concurrency.

Book keeping:
We may need to add additional fields into the file_info struct to keep track of a pipe's state, specifically:
- the allocated pipe's address
- read offset
- write offset
- whether this is a pipe or an inode


## In-depth Analysis and Implementation

### The functions you have to implement
#### fork `kernel/proc.c: fock()`:
- A new entry in the process table must be created via `allocproc`
- User memory must be duplicated via `vspacecopy`
- The trapframe must be duplicated in the new process
- All the opened files must be duplicated in the new process (not as simple as a memory copy)
- Set the state of the new process to be `RUNNABLE`
- Return 0 in the child, while returning the child's pid in the parent

#### exit `kernel/proc.c: exit()`:
- Set the current process’s state to `ZOMBIE`
- Close all open files  (and other resources the child can clean)
- If the process calling exit has any children (parent exits before children), let the initial process reclaim the children’s data
- Wake up the parent process (if any) sleeping on the channel

#### wait `kernel/proc.c: wait()`:
- Check if the parent has any children, return -1 if not
- Iterate through the process table and find one exited child. If found, return the child’s PID; if not, the parent should sleep until one child exits.
- The parent cleans up the exited child’s data (PCB, page tables, kernel stack)

#### pipe `kernel/sysfile.c: sys_pipe()`:
- Creates a pipe using kalloc() and opens two file descriptors, one for reading and one for writing
- Populate the current states of the pipe to the file_info struct, including setting read/write offset to be 0, storing the pipe's address, and indicating it's a pipe
- A write to a pipe with no open read descriptors will return an error
- A read from a pipe with no open write descriptors will return any remaining buffered data, and then 0 to indicate `EOF`.

#### exec `kernel/sysfile.c: sys_exec()`:
- Set up a new virtual address space and new registers states
- Switch to using the new VAS and register states


### Existing functions you need to modify
- sys_open (`kernel/sysfile.c: sys_open()`)
We need to acquire a spinlock when accessing the global file table to protect it from being modified concurrently, and finally release the lock.
- sys_close (`kernel/sysfile.c: sys_close()`)
Acquire a lock on the global file table entry of the file to be closed, so that other processes can modify other files concurrently.
- sys_dup (`kernel/sysfile.c: sys_dup()`)
Acquire a lock on the global file table entry of the file to be duplicated


### Corner cases you will need to handle
#### fork `kernel/proc.c: fock()`:
- kernel lacks space to create new process
#### exit `kernel/proc.c: exit()`:
- The calling process did not create any child processes
- All child processes have been returned in previous calls to wait
#### wait `kernel/proc.c: wait()`:
- The calling process did not create any child processes
- All child processes have been returned in previous calls to wait
#### pipe `kernel/sysfile.c: sys_pipe()`:
- Some address within [arg0, arg0+2*sizeof(int)] is invalid
- Kernel does not have space to create pipe
- Kernel does not have two available file descriptors
- A write to a pipe with no open read descriptors will return an error
- If the pipe fills up before finishing writing, the process should go to sleep and be woken up when another process reads from that pipe
- A read from a pipe with no open write descriptors will return any remaining buffered data, and then 0 to indicate EOF
- If the pipe is empty when reading, the process should go to sleep and be woken up when another process writes from that pipe

#### exec `kernel/sysfile.c: sys_exec()`:
- Path to the executable file points to an invalid or unmapped address
- There is an invalid address before the end of the path to the executable file string
- Path to the executable file is not a valid executable file, or it cannot be opened
- The kernel lacks space to execute the program
- Array of strings for arguments points to an invalid or unmapped address
- There is an invalid address between arg1 and the first n st arg1[n] == '\0' for any i < n, there is an invalid address between arg1[i] and the first `\0'

### Test plan
For fork(), we’ll create a deep copy of the per-process file descriptor table from the parent process and increase the reference count of all open files by 1.
Run `make qemu` to see the output and debug consequently.

## Risk Analysis
### Unanswered question:
What should be the lock like if two pointers point to the same file?
How to reclaim the children’s resources if the parent exit() first?

### Time estimation:
- Best case: 10 h
- Worst case: 20 h
- Average: 15 h

### Staging of work:
First we add synchronization to the system calls implemented in lab 1, specifically `sys_open()`, `sys_close()`, and `sys_dup()`, by providing the global file table and each file_info struct with a spinlock. Then we implement the rest of the system calls: `fork()`, `exit()`, `wait()`, `pipe()`, and `exec()`. Finally, we debug the errors.
