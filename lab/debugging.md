# Debugging Tips

## Trap errors

A common error message you might get is `pid %d %s: trap %d err %d on cpu %d rip 0x%lx addr 0x%x--kill proc` (or `unexpected trap %d from cpu %d rip %lx (cr2=0x%x)`) (note the C format string notation).

These messages are both generated in `trap.c` (around line 77), when a trap was not handled by anything.

We'll start with the first message:

* `pid %d %s` - this tells you which process was running when the trap occurred. For lab 1, this is just going to be 1, but for future labs this could be more useful. The `%s` is the name of the process, which is assigned by kernel; when you implement `fork()`/`exec()` you may find it helpful for debugging purposes to give processes useful names

* `trap %d` - this tells you the trap number. You can see a list of possible values in `trap.h`, but the most common one is going to be 14, which is a page fault.

* `err %d` - this value, for page faults, provides some useful information about the fault. You can see more here, but this is the rundown of the bits you need to worry about: 

    * The least significant bit (`0b1 = 1`) is set when an illegal access was performed on a mapped page (like a write on a read-only page, for example), and is not set when the address is not mapped (i.e., the page doesn't exist). 

    * The second least significant bit (`0b10 = 2`) is set if and only if the fault happened on an attempted write.

    * The third least significant bit (`0b100 = 4`) is set if and only if the fault happened in user mode (e.g., a user program attempted to access 0x0 would have this bit set, while if your kernel code tried to access `0x0` while performing a system call this would not be set). This is almost never set because you write very little user code.

* `cpu %d` - this is the CPU the trap occurred on, which is always going to be 0 in xk, so this is not very useful.

* `rip 0x%lx` - this is the value of %rip that caused the exception, which, you might remember from 351, is the program counter. Using `info line *<value of %rip>` in GDB will give you the line of code that caused the trap.

* `addr 0x%x` - for a page fault, this is the address that was being accessed that triggered the fault. If this is 0x0, you are trying to dereference a null pointer.

The other message is generated if it's an issue with the kernel (look at the code to see the exact conditions that cause this). The information provided is more or less the same (though there is less of it) - the one thing that's useful to know is that `cr2` is a special purpose register (a control register) that contains the address that caused a page fault.

## Panics

Panics are a sort of kernel exception. When one is triggered, you will get a message that looks like this:

```
cpu with apicid 0: panic: Example message
 80100a8b 8010543e 80105337 80105939 80105be1 0 0 0 0 0
```

There a few ways to debug these. One option is to launch xk with GDB. After the panic is triggered, you can type ctrl-C in the GDB window to pause xk. You can then view variables normally, or use `bt` or `backtrace` to see how you got to the panic.

However, when you get a page fault in kernel mode, the backtrace won't lead you to where the invalid address was accessed. Fortunately, some of that data is stored in the sequence of numbers printed after the message. You can use a utility called `line` we added to GDB to print the line information for the backtrace. For example, for the panic above, you could run `line 80100a8b 8010543e 80105337 80105939 80105be1 0 0 0 0 0` and it will print out the lines that each of those addresses is associated with. Note that the panic doesn't perfectly store its backtrace - in testing, we found it may occasionally be off by a few lines. However, it can still be helpful in guiding you to where the error occurred.
