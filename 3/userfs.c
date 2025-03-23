#include "userfs.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

enum {
  BLOCK_SIZE = 512,
  MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
  /** Block memory. */
  char *memory;
  /** How many bytes are occupied. */
  size_t occupied;
  /** Next block in the file. */
  struct block *next;
  /** Previous block in the file. */
  struct block *prev;

  /* PUT HERE OTHER MEMBERS */
};

struct file {
  /** Double-linked list of file blocks. */
  struct block *block_list;
  /**
   * Last block in the list above for fast access to the end
   * of file.
   */
  struct block *last_block;
  /** How many file descriptors are opened on the file. */
  int refs;
  /** File name. */
  char *name;
  /** Files are stored in a double-linked list. */
  struct file *next;
  struct file *prev;

  /* PUT HERE OTHER MEMBERS */

  /** Total offset in the file. */
  size_t file_offset;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
  struct file *file;

  /* Additional fields */

  /** Current block being read/written. */
  struct block *current_block;

  /** Current file offset */
  size_t file_pos;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

struct block *
create_block(struct block *prev, struct block *next) {
  struct block *block = (struct block *) malloc(sizeof(struct block));
  if (block == NULL) {
    return NULL;
  }

  block->memory = (char *) malloc(BLOCK_SIZE);
  if (block->memory == NULL) {
    free(block);
    return NULL;
  }

  block->occupied = 0;
  block->next = next;
  block->prev = prev;

  return block;
}

enum ufs_error_code
ufs_errno()
{
  return ufs_error_code;
}

int
ufs_open(const char *filename, int flags)
{
  // reset error
  ufs_error_code = UFS_ERR_NO_ERR;

  if (filename == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct file *target_file = file_list;
  while (target_file != NULL) {
    if (strcmp(target_file->name, filename) == 0) {
      // strings are equal
      break;
    }
    target_file = target_file->next;
  }
  // if flags are not set
  if (target_file == NULL && !(UFS_CREATE & flags)) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (target_file == NULL && (UFS_CREATE & flags)) {
    struct file *new_file = (struct file *) malloc(sizeof(struct file));
    if (new_file == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    // init new file
    new_file->block_list = NULL;
    new_file->last_block = NULL;
    new_file->refs = 0;
    new_file->file_offset = 0;
    new_file->name = strdup(filename);
    // if filename allocation has failed
    if (new_file->name == NULL) {
      free(new_file);
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    // insert new_file at the head of linked list
    new_file->next = file_list;
    new_file->prev = NULL;
    if (file_list != NULL) {
      file_list->prev = new_file;
    }
    // adding new_file to list, it becomes the head of file_list
    file_list = new_file;
    // now, new_file exists => target_file should be valid
    target_file = new_file;
  }

  // check if there is a free memory for new fd
  // if no, then reallocate others, changing the capacity of fd array
  if (file_descriptor_count == file_descriptor_capacity) {
    int new_fd_cap = 0;
    if (file_descriptor_count == 0) {
      new_fd_cap = 2;
    } else {
      new_fd_cap = file_descriptor_capacity * 2;
    }

    struct filedesc **new_file_descriptors =
      (struct filedesc **) realloc(
          file_descriptors, new_fd_cap * (sizeof(struct filedesc *)));
    if (new_file_descriptors == NULL) {
      // realloc() fails, returned pointer is null
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }

    file_descriptor_capacity = new_fd_cap;
    file_descriptors = new_file_descriptors;
  }

  struct filedesc *new_fd =
    (struct filedesc *) malloc(sizeof(struct filedesc));
  if (new_fd == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  // init new_fd
  // in case of a new file: target_file also points to new_file
  new_fd->file = target_file;
  ++target_file->refs;

  new_fd->file_pos = 0;
  new_fd->current_block = target_file->block_list;

  file_descriptors[file_descriptor_count] = new_fd;
  ++file_descriptor_count;

  return file_descriptor_count - 1;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
  ufs_error_code = UFS_ERR_NO_ERR;

  if (fd < 0 || fd >= file_descriptor_count) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct filedesc *FD = file_descriptors[fd];
  if (FD == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct file *target_file = FD->file;
  if (target_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  size_t written = 0;

  if (target_file->block_list == NULL) {
    // FAT has 512 bytes per block
    // that means each block contains 512 bytes of memory
    // except fields size
    //
    // initial block (0) - the head of block list
    struct block *block = create_block(NULL, NULL);
    if (block == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }
    FD->current_block = block;
    target_file->block_list = block;
    target_file->last_block = block;

    size_t buf_pos = 0;
    size_t remaining = size;

    while (written < size) {
      size_t copy_size = (BLOCK_SIZE > remaining) ?
        remaining : BLOCK_SIZE;

      memcpy(block->memory, buf + buf_pos, copy_size);
      remaining -= copy_size;
      written += copy_size;
      buf_pos += copy_size;
      block->occupied += copy_size;

      FD->file_pos += copy_size;
      if (FD->file_pos > target_file->file_offset) {
        target_file->file_offset = FD->file_pos;
      }

      // if block is full, choose next block
      if (block->occupied == BLOCK_SIZE) {
        struct block *next_block = create_block(block, NULL);
        if (next_block == NULL) {
          ufs_error_code = UFS_ERR_NO_MEM;
          return -1;
        }

        block = next_block;
        FD->current_block = block;
        target_file->last_block = block;
      }
    }
  } else {
    /**
     * case: block_list != NULL
     */

    size_t buf_pos = 0;
    size_t remaining = size;
    size_t block_offset = FD->file_pos % BLOCK_SIZE;
    struct block *block = FD->current_block;

    while (written < size) {
      size_t copy_size = (BLOCK_SIZE - block_offset > remaining) ?
        remaining : BLOCK_SIZE - block_offset;

      memcpy(block->memory + block_offset, buf + buf_pos, copy_size);
      remaining -= copy_size;
      written += copy_size;
      buf_pos += copy_size;
      block_offset += copy_size;
      block->occupied = (block->occupied < block_offset) ?
        block_offset : block->occupied;

      FD->file_pos += copy_size;
      if (FD->file_pos > target_file->file_offset) {
        target_file->file_offset = FD->file_pos;
      }

      // if block is full, choose next block
      if (block_offset == BLOCK_SIZE) {
        struct block *next_block = block->next;
        if (next_block == NULL) {
          next_block = create_block(block, NULL);
          if (next_block == NULL) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
          }
        }
        // update current state
        block = next_block;
        block_offset = 0;
        FD->current_block = block;
        target_file->last_block = block;
      }
    }
  }

  return written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
  ufs_error_code = UFS_ERR_NO_ERR;

  if (fd < 0 || fd >= file_descriptor_count) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct filedesc *FD = file_descriptors[fd];
  if (FD == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct file *target_file = FD->file;
  if (target_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (target_file->block_list == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  struct block *block = FD->current_block;
  if (block == NULL) {
    block = target_file->block_list;
  }

  size_t read = 0;
  size_t buf_pos = 0;
  size_t block_offset = FD->file_pos % BLOCK_SIZE;
  size_t remaining = target_file->file_offset - FD->file_pos;
  if (remaining > size) {
    remaining = size;
  }

  while (remaining > 0 && block != NULL) {
    size_t can_read_from_block = block->occupied - block_offset;
    size_t copy_size = (remaining < can_read_from_block) ?
      remaining : can_read_from_block;

    memcpy(buf + buf_pos, block->memory + block_offset, copy_size);
    read += copy_size;
    buf_pos += copy_size;
    block_offset += copy_size;
    remaining -= copy_size;
    FD->file_pos += copy_size;

    // if block is completely read, choose next block
    if (remaining > 0) {
      block = block->next;
      FD->current_block = block;
      block_offset = 0;

      if (block == NULL) {
        break;
      }
    }
  }

  return read;
}

int
ufs_close(int fd)
{
  ufs_error_code = UFS_ERR_NO_ERR;

  if (fd < 0 || fd >= file_descriptor_count) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  // decrement file ref count of fd
  --file_descriptors[fd]->file->refs;
  file_descriptors[fd]->file = NULL;
  file_descriptors[fd]->current_block = NULL;

  free(file_descriptors[fd]);
  file_descriptors[fd] = NULL;
  --file_descriptor_count;

  return 0;
}

int
ufs_delete(const char *filename)
{
  ufs_error_code = UFS_ERR_NO_ERR;

  if (filename == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct file *target_file = file_list;
  while (target_file != NULL) {
    if (strcmp(target_file->name, filename) == 0) {
      break;
    }
    target_file = target_file->next;
  }

  if (target_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  // remove target file from linked list
  if (target_file->next == NULL) {
    // target file is the tail of the list
    struct file *prev_file = target_file->prev;
    if (prev_file != NULL) {
      prev_file->next = NULL;
    } else {
      // file_list is empty now
      file_list = NULL;
    }
  } else if (target_file->prev == NULL) {
    // target file is the head of the list
    struct file *next_file = target_file->next;
    if (next_file != NULL) {
      next_file->prev = NULL;
    }
    file_list = next_file;
  } else {
    // target file is in the middle of list
    struct file *prev_file = target_file->prev;
    struct file *next_file = target_file->next;
    prev_file->next = next_file;
    next_file->prev = prev_file;
  }

  target_file->prev = NULL;
  target_file->next = NULL;

  if (target_file->refs == 0) {
    // TODO: clear all data in blocks
    //
    free(target_file->name);
    free(target_file);
  }

  // otherwise, the file data continues to exist
  // as long as at least one descriptor exists

  return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
  /* IMPLEMENT THIS FUNCTION */
  (void)fd;
  (void)new_size;
  ufs_error_code = UFS_ERR_NOT_IMPLEMENTED;
  return -1;
}

#endif

void
ufs_destroy(void)
{
}
