#include "userfs.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
  struct file *file;

  /** Current block being read/written. */
  struct block *current_block;
  /** Current position within the current block. */
  size_t block_offset;
  /** Total offset in the file. */
  size_t file_offset;
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

  struct file *existing_file = file_list;
  while (existing_file != NULL) {
    if (strcmp(existing_file->name, filename) == 0) {
      // strings are equal
      break;
    }
    existing_file = existing_file->next;
  }

  // if flags are not set
  if (existing_file == NULL && !(UFS_CREATE & flags)) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (existing_file == NULL && (UFS_CREATE & flags)) {
    struct file *new_file = (struct file *) malloc(sizeof(struct file));
    if (new_file == NULL) {
      ufs_error_code = UFS_ERR_NO_MEM;
      return -1;
    }

    // init new file
    new_file->block_list = NULL;
    new_file->last_block = NULL;
    new_file->refs = 0;
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
    // now, new_file - head of file_list
    file_list = new_file;
  }

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

    file_descriptor_capacity = new_fd_cap;
    file_descriptors = new_file_descriptors;
  }

  struct filedesc *new_fd = (struct filedesc *) malloc(sizeof(struct filedesc));
  if (new_fd == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  // init new_fd
  new_fd->block_offset = 0;
  new_fd->current_block = 0;
  new_fd->file_offset = 0;

  new_fd->file = file_list;
  ++file_list->refs;

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

  if (file_descriptors[fd] == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  struct file *target_file = file_descriptors[fd]->file;
  // perhaps an unnecessary check
  // but should false always (make sure, I made it correctly)
  if (target_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (target_file->block_list == NULL) {
    size_t quotient = size / BLOCK_SIZE;
    size_t remainder = size % BLOCK_SIZE;
    if (remainder > 0) {
      quotient += 1;
    }

    // as I found that FAT has 512 bytes per block
    // so, each block contains 512 bytes of memory
    // except fields size
    //
    // initial block (0)
    struct block *block = (struct block *) malloc(sizeof(struct block));
    block->memory = (char *) malloc(BLOCK_SIZE);
    block->occupied = 0;
    block->next = NULL;
    block->prev = NULL;
    // initial block would be the tail of list
    target_file->last_block = block;

    struct block *next_block = NULL;
    for (size_t i = 1; i < quotient; ++i) {
      // inserting new block at the head
      next_block = (struct block *) malloc(sizeof(struct block));
      next_block->memory = (char *) malloc(BLOCK_SIZE);
      next_block->occupied = 0;
      next_block->next = block;
      next_block->prev = NULL;
      // chain tail block
      block->prev = next_block;
      // replace block by next_block
      block = next_block;
    }
    // now, block and next_block store
    // pointer to first block
    // NOTE: in case of multiple blocks
    //
    // if it is only one block then
    // only variable `block` would be valid
    target_file->block_list = block;

    // fill blocks with data
    size_t written = 0;
    size_t copy_size = 0;
    while (written < size) {
      copy_size = BLOCK_SIZE - block->occupied;
      if (copy_size > size) {
        copy_size = size;
      }
      memcpy(block->memory, buf, copy_size);
      block->occupied += copy_size;
      written += copy_size;

      // if block is full, choose next block
      if (block->occupied == BLOCK_SIZE) {
        block = block->next;
      }

      // TODO: add offset to fd
    }

    // TODO: implement for not empty files

    return written;
  }

  return -1;
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

  struct file *target_file = FD ->file;
  if (target_file == NULL) {
    ufs_error_code = UFS_ERR_NO_FILE;
    return -1;
  }

  if (target_file->block_list == NULL) {
    ufs_error_code = UFS_ERR_NO_MEM;
    return -1;
  }

  size_t read = 0;
  size_t copy_size = 0;
  struct block *block = target_file->block_list;
  FD->current_block = block;
  while (block != NULL && FD->block_offset != block->occupied) {
    copy_size = size;
    if (copy_size > block->occupied) {
      copy_size = block->occupied;
    }
    memcpy(buf, block->memory, copy_size);
    FD->block_offset += copy_size;
    read += copy_size;
    size -= read;

    // if block is full, choose next block
    if (FD->block_offset == BLOCK_SIZE) {
      block = block->next;
      FD->block_offset += 1;
      FD->current_block = block;
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
