#include "userfs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // printf("alloc count before start: %d\n", heaph_get_alloc_count());

    // write & read 512 bytes
    printf("-- 512 bytes test start --\n");
	int fd_512 = ufs_open("file_512", UFS_CREATE);
	assert(fd_512 != -1);
    int buff_size = 513;
	char *buff = (char *) malloc(buff_size);
	for (int i = 0; i < buff_size; ++i)
		buff[i] = 'a' + i % 26;

    ssize_t rc = ufs_write(fd_512, buff, buff_size);
    assert(rc == buff_size);

    int fd_512_1 = ufs_open("file_512", 0);
    char *buf_out = (char *) malloc(buff_size);
    ufs_read(fd_512_1, buf_out, buff_size);
    assert(memcmp(buf_out, buff, buff_size) == 0);

    free(buff);
    free(buf_out);
    ufs_close(fd_512);
    ufs_close(fd_512_1);

    // stress test
	const int count = 1000;
	int fd[count][2];
	char name[16], buf[16];
	printf("open %d read and write descriptors, fill with data\n", count);
	for (int i = 0; i < count; ++i) {
		int name_len = sprintf(name, "file%d", i) + 1;
		int *in = &fd[i][0], *out = &fd[i][1];
		*in = ufs_open(name, UFS_CREATE);
		*out = ufs_open(name, 0);
		assert(*in != -1 && *out != -1);
		ssize_t rc = ufs_write(*out, name, name_len);
		assert(rc == name_len);
	}
	printf("read the data back\n");
	for (int i = 0; i < count; ++i) {
		int name_len = sprintf(name, "file%d", i) + 1;
		int *in = &fd[i][0], *out = &fd[i][1];
		ssize_t rc = ufs_read(*in, buf, sizeof(buf));
		assert(rc == name_len);
		assert(memcmp(buf, name, rc) == 0);
		assert(ufs_close(*in) == 0);
		assert(ufs_close(*out) == 0);
		assert(ufs_delete(name) == 0);
	}

    ufs_delete("file_512");
    ufs_destroy();
}
