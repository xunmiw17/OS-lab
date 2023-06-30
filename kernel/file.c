//
// File descriptors
//

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <proc.h>
#include <fcntl.h>
#include <stat.h>

struct devsw devsw[NDEV];

struct file_info global_arr[NFILE];
struct spinlock lk;

int dup(int fd) {
  struct proc* proc = myproc();
  struct file_info* file = proc->file_info[fd];

  // The file descriptor represents a pipe
  if (file->inode == 0) {
    struct pipe_info* pi = file->pipe_info;
    // Allocate the lowest available file descriptor
    for (int i = 0; i < NOFILE; i++) {
      if (proc->file_info[i] == NULL) {
        // Increase the read/write reference by 1
        acquire(&pi->lock);
        if (file->mode == O_RDONLY) {
          pi->read_end_open++;
        } else {
          pi->write_end_open++;
        }
        release(&pi->lock);
        // Increase the reference count by 1
        acquire(&lk);
        file->refer_count += 1;
        release(&lk);
        proc->file_info[i] = file;
        return i;
      }
    }
  } else {
    // Allocate the lowest available file descriptor
    for (int i = 0; i < NOFILE; i++) {
      if (proc->file_info[i] == NULL) {
        // Increase the reference count by 1
        locki(file->inode);
        file->refer_count += 1;
        unlocki(file->inode);
        proc->file_info[i] = file;
        return i;
      }
    }
  }
  return -1;
}

int read(int fd, char* buf, int num) {
  if (num < 0)
    return -1;

  struct file_info* file_read = myproc()->file_info[fd];

  int bytes_read;
  // The file descriptor represents a pipe
  if (file_read->inode == 0) {
    if (file_read->mode != O_RDONLY)
      return -1;
    struct pipe_info* pi = file_read->pipe_info;
    acquire(&pi->lock);
    // Block until there're bytes to read
    while (pi->read_offset == pi->write_offset) {
      // The write end has been closed
      // Can check this only when there is no more bytes to read. When there are
      // still bytes to read, it will read these remaining bytes.
      if (pi->write_end_open == 0) {
        release(&pi->lock);
        return 0;
      }
      sleep(pi->pipe, &pi->lock);
    }

    // Calculate how much to read
    if (pi->write_offset > pi->read_offset) {
      bytes_read = pi->write_offset - pi->read_offset;
    } else {
      bytes_read = PIPELEN - (pi->read_offset - pi->write_offset);
    }
    if (num < bytes_read) {
      bytes_read = num;
    }

    // Read data from pipe to buffer
    for (int i = 0; i < bytes_read; i++) {
      buf[i] = pi->pipe[(pi->read_offset + i) % PIPELEN];
    }

    pi->read_offset = (pi->read_offset + bytes_read) % PIPELEN;
    pi->num_avail += bytes_read;
    // Wake up the writers
    wakeup(pi->pipe);
    release(&pi->lock);
  } else {
    // Check if the file descriptor is open for read
    locki(file_read->inode);
    if (file_read->mode != O_RDONLY && file_read->mode != O_RDWR) {
      unlocki(file_read->inode);
      return -1;
    }

    bytes_read = readi(file_read->inode, buf, file_read->offset, num);
    if (bytes_read == -1) {
      unlocki(file_read->inode);
      return -1;
    }
    file_read->offset += bytes_read;
    unlocki(file_read->inode);
  }

  return bytes_read;
}

int write(int fd, char* p, int n) {
    // number of bytes to write is not positive
  if (n < 0)
    return -1;

  struct file_info* file_write = myproc()->file_info[fd];

  int bytes_write;
  // The file descriptor represents a pipe
  if (file_write->inode == 0){
    if (file_write->mode != O_WRONLY)
      return -1;
    struct pipe_info* pi = file_write->pipe_info;
    acquire(&pi->lock);
    // The read end has been closed
    // The writer must check this in both cases: when there are remaining bytes
    // in the pipe, or when the pipe is empty (checked later)
    if (pi->read_end_open == 0) {
      release(&pi->lock);
      return -1;
    }
    // Block until there're bytes to write
    while (pi->num_avail == 0) {
      // The read end has been closed
      if (pi->read_end_open == 0) {
        release(&pi->lock);
        return -1;
      }
      sleep(pi->pipe, &pi->lock);
    }

    // Calculate how much to write to the pipe
    if (n > pi->num_avail) {
      bytes_write = pi->num_avail;
    } else {
      bytes_write = n;
    }

    // Write data from buffer to pipe
    for (int i = 0; i < bytes_write; i++) {
      pi->pipe[(pi->write_offset + i) % PIPELEN] = p[i];
    }

    pi->write_offset = (pi->write_offset + bytes_write) % PIPELEN;
    pi->num_avail -= bytes_write;
    // Wake up the readers
    wakeup(pi->pipe);
    release(&pi->lock);
  } else {
    // Check if the file descriptor is open for write
    locki(file_write->inode);
    if (file_write->mode != O_WRONLY && file_write->mode != O_RDWR) {
      unlocki(file_write->inode);
      return -1;
    }

    log_begin_tx();
    bytes_write = writei(file_write->inode, p, file_write->offset, n);
    log_commit_tx();

    if (bytes_write == -1) {
      unlocki(file_write->inode);
      return -1;
    }
    file_write->offset += bytes_write;
    unlocki(file_write->inode);
  }

  return bytes_write;
}

int close(int fd) {
  struct file_info* file_clo = myproc()->file_info[fd];

  // The file descriptor represents a pipe
  if (file_clo->inode == 0) {
    acquire(&file_clo->pipe_info->lock);
    struct pipe_info* pi = file_clo->pipe_info;
    // Decrement the read/write reference by 1
    if (file_clo->mode == O_RDONLY) {
      pi->read_end_open--;
    } else {
      pi->write_end_open--;
    }
    // Wake up the other end since there may now be no longer readers/writers
    wakeup(pi->pipe);

    int can_free_pipe = pi->read_end_open == 0 && pi->write_end_open == 0;
    release(&file_clo->pipe_info->lock);

    // If there are no readers and no writers, free the pipe
    if (can_free_pipe) {
      kfree((char*) pi);
    }

    // Update file_info metadata
    // Since inode is NULL, we can only acquire the global file table lock
    acquire(&lk);
    file_clo->refer_count--;
    if (file_clo->refer_count == 0) {
      file_clo->mode = 0;
      file_clo->pipe_info = NULL;
    }
    myproc()->file_info[fd] = NULL;
    release(&lk);
  } else {
    acquire(&lk);
    file_clo->refer_count -= 1;

    if (file_clo->refer_count == 0) {
      irelease(file_clo->inode);
      file_clo->inode = NULL;
      file_clo->offset = 0;
      file_clo->mode = 0;
    }

    myproc()->file_info[fd] = NULL;
    release(&lk);
  }
  return 0;
}

int fstat(int fd, struct stat* stat) {
  concurrent_stati(myproc()->file_info[fd]->inode, stat);
  return 0;
}

int open(char* file_path, int mode) {
  struct inode* inode = namei(file_path);
  if (inode == 0 && (mode & O_CREATE) == O_CREATE) {
    log_begin_tx();
    inode = createi(file_path);
    log_commit_tx();
  } else if (inode == 0) {
    return -1;
  }

  locki(inode);
  unlocki(inode);

  if (mode != O_RDONLY && mode != O_WRONLY && mode != O_RDWR && mode != (O_CREATE | O_RDONLY) && mode != (O_CREATE | O_WRONLY) && mode != (O_CREATE | O_RDWR))
    return -1;

  // Find an empty spot in the per-process file struct table
  int found = 0;
  int i;
  for (i = 0; i < NOFILE; i++) {
    if (myproc()->file_info[i] == NULL) {
      found = 1;
      break;
    }
  }

  if (!found) {
    return -1;
  }

  acquire(&lk);
  // Find an empty spot in the global file table
  for (int j = 0; j < NFILE; j++) {
    if (global_arr[j].refer_count == 0) {
      global_arr[j].inode = inode;
      global_arr[j].offset = 0;
      global_arr[j].mode = (mode & ~O_CREATE);
      global_arr[j].refer_count = 1;
      myproc()->file_info[i] = &global_arr[j];
      release(&lk);
      return i;
    }
  }
  release(&lk);

  return -1;
}

int pipe(int* arr) {
  // Allocates a pipe_info struct to be shared between the read and write end
  // of a pipe
  struct pipe_info* pipe_info = (struct pipe_info*) kalloc();
  if (pipe_info == 0)
    return -1;

  // Allocates a pipe_info struct to be shared between the read and write end
  // of a pipe
  pipe_info->read_offset = 0;
  pipe_info->write_offset = 0;
  pipe_info->read_end_open = 1;
  pipe_info->write_end_open = 1;
  pipe_info->num_avail = PIPELEN;
  initlock(&pipe_info->lock, "pipe lock");

  int read_fd, write_fd;
  int num_fds = 0;
  // Allocate two file descriptors in the per-process file table
  for (int i = 0; i < NOFILE; i++) {
    if (num_fds == 2)
      break;
    if (myproc()->file_info[i] == NULL) {
      if (num_fds == 0) {
        read_fd = i;
      } else {
        write_fd = i;
      }
      num_fds++;
    }
  }

  // Kernel does not have two available file descriptors
  if (num_fds != 2) {
    kfree((char*) pipe_info);
    return -1;
  }

  num_fds = 0;

  // Allocate two file structs to represent pipe in the global file table
  acquire(&lk);
  for (int i = 0; i < NFILE; i++) {
    if (num_fds == 2)
      break;
    if (global_arr[i].refer_count == 0) {
      global_arr[i].refer_count = 1;
      global_arr[i].pipe_info = pipe_info;
      if (num_fds == 0) {
        global_arr[i].mode = O_RDONLY;
        myproc()->file_info[read_fd] = &global_arr[i];
      } else {
        global_arr[i].mode = O_WRONLY;
        myproc()->file_info[write_fd] = &global_arr[i];
      }
      num_fds++;
    }
  }
  release(&lk);

  // The global file table does not have two available file structs
  if (num_fds != 2) {
    kfree((char*) pipe_info);
    return -1;
  }

  arr[0] = read_fd;
  arr[1] = write_fd;
  return 0;
}

// Removes the file from the file system
int unlink(char* file_path) {
  struct inode* inode = namei(file_path);
  if (inode == 0 || inode->type == T_DIR || inode->type == T_DEV || inode->ref > 1) {
    if (inode != 0) {
      irelease(inode);
    }
    return -1;
  }
  irelease(inode);
  log_begin_tx();
  int res = unlinki(file_path);
  log_commit_tx();
  return res;
}