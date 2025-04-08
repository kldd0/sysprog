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

    /** Flags */
    unsigned int flags;
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

struct file *
create_file(struct file *prev, struct file *next, const char *filename) {
    struct file *file = (struct file *) malloc(sizeof(struct file));
    if (file == NULL) {
        return NULL;
    }

    file->block_list = NULL;
    file->last_block = NULL;
    file->next = next;
    file->prev = prev;
    file->refs = 0;
    file->file_offset = 0;

    file->name = strdup(filename);
    if (file->name == NULL) {
        free(file);
        return NULL;
    }

    if (next != NULL) {
        next->prev = file;
    }

    if (prev != NULL) {
        prev->next = file;
    }

    return file;
}

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

    // Add link to current block
    if (prev != NULL) {
        prev->next = block;
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
    // Reset error
    ufs_error_code = UFS_ERR_NO_ERR;

    if (filename == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct file *target_file = file_list;
    while (target_file != NULL) {
        if (strcmp(target_file->name, filename) == 0) {
            // Strings are equal
            break;
        }
        target_file = target_file->next;
    }
    // If flags are not set
    if (target_file == NULL && !(UFS_CREATE & flags)) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (target_file == NULL && (UFS_CREATE & flags)) {
        // Inserting a new file to the top of the file list
        // by specifying file_list as the next file node
        struct file *file = create_file(NULL, file_list, filename);
        if (file == NULL) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        file_list = file;
        target_file = file;
    }

    // Check if there is a free memory for new fd
    // if no, then reallocate others, changing the capacity of fd array
    if (file_descriptor_count == file_descriptor_capacity) {
        int new_fd_cap = (file_descriptor_count == 0) ?
            2 : file_descriptor_capacity * 2;

        struct filedesc **new_file_descriptors =
            (struct filedesc **) realloc(
                    file_descriptors, new_fd_cap * sizeof(struct filedesc *));
        if (new_file_descriptors == NULL) {
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

    // In case of a new file, target_file also points to new_file
    new_fd->file = target_file;
    ++target_file->refs;

    new_fd->file_pos = 0;
    new_fd->flags = flags;
    new_fd->current_block = target_file->block_list;

    file_descriptors[file_descriptor_count] = new_fd;
    ++file_descriptor_count;

    return file_descriptor_count - 1;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd < 0 || fd >= file_descriptor_capacity) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *FD = file_descriptors[fd];
    if (FD == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (FD->flags & UFS_READ_ONLY) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    struct file *target_file = FD->file;
    if (target_file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    // NOTE:
    // FAT has 512 bytes per block,
    // it means that each block contains
    // 512 bytes of memory except other fields size

    struct block *block = FD->current_block;
    size_t written = 0;
    size_t buf_pos = 0;
    size_t remaining = size;
    size_t block_offset = FD->file_pos % BLOCK_SIZE;

    if (block == NULL) {
        // Case when file is newly created:
        // init the head of block list
        block = create_block(NULL, NULL);
        if (block == NULL) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

        FD->current_block = block;
        target_file->block_list = block;
        target_file->last_block = block;
    }

    if (FD->file_pos > target_file->file_offset) {
        // Finding current block after file resize
        FD->file_pos = target_file->file_offset;
        block_offset = FD->file_pos % BLOCK_SIZE;
        block = target_file->block_list;
        size_t offset = 0;

        while (offset + BLOCK_SIZE < FD->file_pos) {
            block = block->next;
            offset += BLOCK_SIZE;
        }

        FD->current_block = block;
    }

    // The necessary check is for the case when the block was previously written
    // with data of length 2^n. (last block is still the same)
    if (FD->file_pos != 0 && block_offset == 0) {
        block_offset = BLOCK_SIZE;
    }

    while (written < size) {
        size_t copy_size = (BLOCK_SIZE - block_offset > remaining) ?
            remaining : BLOCK_SIZE - block_offset;

        if ((FD->file_pos + copy_size) > MAX_FILE_SIZE) {
            ufs_error_code = UFS_ERR_NO_MEM;
            return -1;
        }

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

        // If block is full, choose next block
        if (block_offset == BLOCK_SIZE && remaining > 0) {
            struct block *next_block = block->next;
            if (next_block == NULL) {
                next_block = create_block(block, NULL);
                if (next_block == NULL) {
                    ufs_error_code = UFS_ERR_NO_MEM;
                    return -1;
                }
            }
            // Update current state
            block = next_block;
            FD->current_block = block;
            target_file->last_block = block;
            // Reset block_offset for the new block
            block_offset = 0;
        }
    }

    return written;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd < 0 || fd >= file_descriptor_capacity) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *FD = file_descriptors[fd];
    if (FD == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (FD->flags & UFS_WRITE_ONLY) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    struct file *target_file = FD->file;
    if (target_file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (target_file->block_list == NULL) {
        return 0;
    }

    if (FD->current_block == NULL) {
        FD->current_block = target_file->block_list;
    }

    struct block *block = FD->current_block;

    if (FD->file_pos > target_file->file_offset) {
        // Finding current block after file resize
        FD->file_pos = target_file->file_offset;
        block = target_file->block_list;
        size_t offset = 0;

        while (offset + BLOCK_SIZE < FD->file_pos) {
            block = block->next;
            offset += BLOCK_SIZE;
        }

        FD->current_block = block;
    }

    size_t read = 0;
    size_t buf_pos = 0;
    size_t block_offset = FD->file_pos % BLOCK_SIZE;
    size_t remaining = target_file->file_offset - FD->file_pos;

    if (remaining > size) {
        remaining = size;
    }

    // WHY it should be removed
    // if (FD->file_pos != 0 && block_offset == 0) {
    //     block_offset = BLOCK_SIZE;
    // }

    while (remaining > 0 && block != NULL) {
        size_t can_read_from_block = block->occupied - block_offset;
        size_t copy_size = (remaining < can_read_from_block) ?
            remaining : can_read_from_block;

        memcpy(buf + buf_pos, block->memory + block_offset, copy_size);
        remaining -= copy_size;
        read += copy_size;
        buf_pos += copy_size;
        block_offset += copy_size;
        FD->file_pos += copy_size;

        // If block is completely read, choose next block
        if (block_offset == BLOCK_SIZE) {
            block = block->next;
            if (block == NULL) {
                break;
            }

            block_offset = 0;
            FD->current_block = block;
        }
    }

    return read;
}

int
ufs_close(int fd)
{
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd < 0 || fd >= file_descriptor_capacity) {
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

    // Cycle above guarantee that file_list is not NULL
    struct file *prev = target_file->prev;
    struct file *next = target_file->next;

    if (prev != NULL) {
        prev->next = next;
    }

    if (next != NULL) {
        next->prev = prev;
    }

    if (file_list == target_file) {
        file_list = next;
    }

    target_file->prev = NULL;
    target_file->next = NULL;

    if (target_file->refs == 0) {
        free(target_file->name);

        struct block *block = target_file->block_list;
        while (block != NULL) {
            struct block *next_block = block->next;

            free(block->memory);
            free(block);

            block = next_block;
        }

        target_file->last_block = NULL;
        target_file->block_list = NULL;
        free(target_file);
    }

    // Otherwise, the file data continues to exist
    // as long as at least one descriptor exists

    return 0;
}

#if NEED_RESIZE

int
ufs_resize(int fd, size_t new_size)
{
    ufs_error_code = UFS_ERR_NO_ERR;

    if (fd < 0 || fd >= file_descriptor_capacity) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    struct filedesc *FD = file_descriptors[fd];
    if (FD == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (FD->flags & UFS_WRITE_ONLY || FD->flags & UFS_READ_WRITE) {
        ufs_error_code = UFS_ERR_NO_PERMISSION;
        return -1;
    }

    if (new_size > MAX_FILE_SIZE) {
        ufs_error_code = UFS_ERR_NO_MEM;
        return -1;
    }

    struct file *target_file = FD->file;
    if (target_file == NULL) {
        ufs_error_code = UFS_ERR_NO_FILE;
        return -1;
    }

    if (target_file->block_list == NULL) {
        return 0;
    }

    struct block *block = target_file->last_block;

    if (new_size < target_file->file_offset) {
        size_t remaining = target_file->file_offset - new_size;

        while (remaining > 0 && block != NULL) {
            size_t deletion_size = (remaining < block->occupied) ?
                remaining : block->occupied;

            remaining -= deletion_size;
            block->occupied -= deletion_size;
            target_file->file_offset -= deletion_size;

            if (block->occupied == 0) {
                struct block *prev_block = block->prev;
                free(block->memory);
                free(block);
                block = prev_block;

                if (block != NULL) {
                    block->next = NULL;
                }
            }
        }

        target_file->last_block = block;
        // Case when new_size == 0
        if (block == NULL) {
            target_file->block_list = NULL;
        }
    } else {
        size_t remaining = new_size - target_file->file_offset;

        while (remaining > 0) {
            size_t can_resize_within_block = BLOCK_SIZE - block->occupied;
            size_t update_size = (remaining < can_resize_within_block) ?
                remaining : can_resize_within_block;

            remaining -= update_size;
            target_file->file_offset += update_size;

            if (target_file->file_offset % BLOCK_SIZE == 0 && remaining > 0) {
                struct block *next_block = block->next;
                if (next_block == NULL) {
                    next_block = create_block(block, NULL);
                    if (next_block == NULL) {
                        ufs_error_code = UFS_ERR_NO_MEM;
                        return -1;
                    }
                }

                block = next_block;
                target_file->last_block = block;
            }
        }
    }

    return 0;
}

#endif

void
ufs_destroy(void)
{
    for (int i = 0; i < file_descriptor_count; ++i) {
        ufs_close(i);
        free(file_descriptors[i]);
    }
    free(file_descriptors);

    struct file *file = file_list;
    while (file != NULL) {
        struct file *next_file = file->next;
        ufs_delete(file->name);
        file = next_file;
    }
}
