# Lab 4 Design Doc: File System

## Overview
The goal of this assignment is to extend the functionality of the file system to allow file creation, file write, and file deletion, which involve modifying the system call `open()` and adding the `unlink()` system call. In addition, we need to build a crash-safe file system by creating a logging layer in the disk layout, which temporarily stores all the written data before flushing them to their intended locations.

### Major parts

- File write:
    - Modify the function `writei()` in fs.c so that inodes can be used to write back to disk.
    - Use `bread()`, `bwrite()`, and `brelse()` to write the data to disk.
    - Modify the `open()` system call to support O_RDWR for read/write access to files.

- File Append:
    - If writing at the end of a file, its size should grow
    - Should support multiple extents for each file to allow file append. Need to allocate extra space using `balloc()` and let the new extents point to those blocks.
    - Update the dinode with the new size

- File Create:
    - Be able to create a new file when O_CREATE is passed to `open()`.
    - Create a new dinode and append it to the inode file
    - Update root directory to reference this new dinode
    - Update bitmap to reflect the newly allocated dinode

- File Delete:
    - Given a pathname for a file, if no process has an open reference to the file, `sys_unlink()` removes the file from the file system.
    - Undo steps from file creation
    - On success, returns 0. On error, returns -1.

- Crash-safe file system:
    - Add a logging layer in the disk layout.
    - For any operation which must write multiple disk blocks atomically:
        - Clear out any data currently in the log
        - Write new blocks into the log, rather than the target place. At the same time, track where the target is.
        - Once all blocks are in the log, mark the log as “committed”
        - Copy data from the log to where they should be

Bookkeeping: We need to modify one field for both `struct inode` and `struct dinode`:

    struct extent data[30];

- In this way, we support multiple extents for each file. Additionally, to keep the size of `struct dinode` to be a power of 2, we change its padding field to be “char pad[8];”.

To allow logging, we also need to add a logging layer between the bitmap and inodes layer. Besides, we define two new structures to achieve better log formatting in a new file `journal.h`:

    struct log_header {
        int committed;
        int nblocks;
    };

    struct block_info {
        uint blockno;
    };

- where `struct log_header` keeps track of the metadata of the log, including whether the log has been committed and the total number of data blocks written. This information is put at the first block of the logging layer. `struct block_info` keeps track of the actual block number to write to disk for every data block. This information occupies its own block and precedes the data block.

In addition, we should change the superblock to add a new logging region:

    struct superblock {
        uint size;
        uint nblocks;
        uint bmapstart;
        uint logstart;  // the starting block for the logging region
        uint inodestart;
    };



## In-depth Analysis and Implementation

### The functions you have to implement
- sys_unlink `kernel/sysfile.c: sys_unlink()`:
    - If the file does not exist, or the path represents a directory or device, or the file currently has an open reference, return -1.
    - Use `nameiparent (kernel/fs.c)` to find the inode for the root directory. Lock the root directory inode using `locki (kernel/fs.c)`. Loop through the root directory and read each directory entry using `readi (kernel/fs.c)` to find a matching file name. When found, store the inum field and set it to -1, indicating this inode has been deleted. Write the updated dinode back to disk using `writei (kernel/fs.c)`. Reduce the size metadata of the root directory by `sizeof(struct dirent)`. Release the lock through `unlocki (kernel/fs.c)`.
    - Lock the inode file by `locki(&icache.inodefile)`. Create a dinode with type set to -1 as a placeholder to indicate the file is deleted. Write this dinode to the location of the dinode to be deleted (using the inum field stored in the last step) using `writei (kernel/fs.c)`. Reduce the size metadata of the inode file by `sizeof(struct dinode)`. Finally release the lock through `unlocki(&icache.inodefile)`.

- log_begin_tx `kernel/journal.c: log_begin_tx()`
    - Construct a new `struct log_header`. Set both its committed field and nblocks field to be 0.
    - Get the first block of the logging layer using `bread(ROOTDEV, sb.logstart) (kernel/bio.c)`. Move the constructed struct log_header to that block using `memmove (kernel/string.c)`. Write and release the buffer using `bwrite (kernel/bio.c)` and `brelse (kernel/bio.c)`.

- log_write `kernel/journal.c: log_write()`:
    - Get how many data blocks are there in the logging layer by reading the nblocks field of the log header using `bread(ROOTDEV, sb.logstart) (kernel/bio.c)`.
    - Using the extracted nblocks, we can find the location to write the block number through sb.logstart + 1 + 2 * nblocks. Write the block number (b->blockno) to that location using `bread`, `bwrite`, and `brelse (kernel/bio.c)` and `memmove (kernel/string.c)`.
    - Get the location to write the data block through sb.logstart + 1 + 2 * nblocks + 1. Write the data block (b->data) to that location using `bread`, `bwrite`, and `brelse (kernel/bio.c)` and `memmove (kernel/string.c)`.
    - Increment the nblocks field in the log header by 1. Write the updated header back to disk using bwrite, and `brelse (kernel/bio.c)` and `memmove (kernel/string.c)`.

- log_commit_tx `kernel/journal.c: log_commit_tx()`:
    - Extract the log header and update the commit flag using the same steps. Also store the nblocks field.
    - Using nblocks, loop through all block number and data block pairs in the logging layer. At each iteration, get the block number and data block using the same steps. Finally, using the block number blockno we get, we write the data to its actual location by first calling `bread(ROOTDEV, blockno) (kernel/bio.c)` to read the actual block. We then write the data using `memmove (kernel/string.c)` and at last call `bwrite` and `brelse (kernel/bio.c)`.
    - Zero out the commit flag using the same steps.

- log_recover `kernel/journal.c: log_recover()`:
    - Get the log header using the same steps. Store the nblocks field.
    - Check the commit flag of the log header. If it is 1, copy all the data blocks to their actual disk locations and zero out the commit flag using the same steps as before. If it is 0, we do nothing (but need to call `brelse (kernel/bio.c)` on the log header buffer)

### Existing functions/files you need to modify
- mkfs.c; init_inodefile (`kernel/fs.c: init_inodefile()`); locki (`kernel/fs.c: locki()`)
    - For these 3 files, adjust the dinode’s/inode’s data field accordingly since there are now multiple extents instead of just one.
    - Besides, for mkfs.c, we need to accommodate to the new logging region
- readi (`kernel/fs.c: readi()`)
    - Given the offset, we first calculate in which extent it lies before actually reading the data. To do this, we keep a count curr_size and go through the file’s extents in order until curr_size is larger than the offset. Then we recalculate the offset to be the offset within that extent.
    - In the for loop in the last part, we need to add a condition to check if the current extent has been completely read (off == 8 * BSIZE). If it is, we jump to the next extent and reset the offset to be 0.
- writei (`kernel/fs.c: writei()`)
    - If we need to append (off + n > ip->size), we first calculate how many more bytes we need to allocate, and then calculate how many more extents should be allocated accordingly (we make each extent hold 8 blocks to make the math easier). Allocate extents using `balloc (kernel/fs.c)`. Update ip->data to reflect the newly added extents.
    - As with `readi (kernel/fs.c)`, we calculate in which extent the offset lies and recalculate the offset to be within that extent.
    - As with `readi (kernel/fs.c)`, in the for loop in the last part, we need to add a condition to check if the current extent has been completely read (off == 8 * BSIZE). If it is, we jump to the next extent and reset the offset to be 0.
- open (`kernel/file.c: open()`)
    - Allow file write.
        - If the mode is O_RDWR or O_WRONLY, don’t return -1 but continue instead.
    - Allow file create
        - If the inode for the given file path does not exist and an O_CREATE flag is set, create an inode (before allocating a file descriptor for it):
            - Allocate a `struct dinode` and set its fields accordingly
            - Acquire the lock on the inode file (icache.inodefile) using `locki (kernel/fs.c)`, then read each dinode in the inode file using `read_dinode (kernel/fs.c)` to find a free dinode (type == -1) and store the inum. If not found, set the inum to be the current number of inodes in the inode file (the end). Write the allocated dinode to that location using `writei (kernel/fs.c)`. Increase the inode file size metadata by `sizeof(struct dinode)`. Release the lock on the inode file using `unlocki (kernel/fs.c)`.
            - Use `nameiparent (kernel/fs.c)` to get the inode for the root directory. Allocate a struct dirent and set its inum field to be the inum got in the last step and name field to be the output parameter of `nameiparent (kernel/fs.c)`. Lock the root directory inode using `locki (kernel/fs.c)`. Go through the root directory and read each directory entry using `readi (kernel/fs.c)` to find a free dirent (inum == -1) and store that index. If not found, set the index to be the current number of directory entries in the root directory (the end). Write the allocated struct dirent to that location using `writei (kernel/fs.c)`. Increase the root directory size metadata by `sizeof(struct dirent)`. Release the lock on the root directory using `unlocki (kernel/fs.c)`.
            - Use `iget (kernel/fs.c)` to get the in-memory copy of the newly allocated dinode so it can be assigned a file descriptor.

## Risk Analysis
### Unanswered question:
- What to do if the data to be written exceeds the size of the log region?

### Time estimation:
- Best case: 20 h
- Worst case: 40 h
- Average: 30 h

### Staging of work:
We first allow file write by modifying struct dinode, struct inode, and other functions such as `readi()`, `init_inodefile()` and `locki()` correspondingly, implementing `writei()`, and modifying the open system call to allow write modes. Then, we implement file append and file create by modifying `writei()` and the `open()` system call. Then, we implement file deletion - complete the `unlink()` system call. We should also modify `mkfs.c` to accommodate the new disk layout. Finally, we achieve a crash-safe file system by implementing `log_begin_tx()`, `log_write()`, `log_commit_tx()`, and `log_recover()`.
