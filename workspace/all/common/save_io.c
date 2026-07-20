#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "save_io.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#ifdef SAVE_IO_TEST
static struct {
	enum SaveIOTestOp op;
	unsigned fail_call;
	unsigned calls;
	int error;
	size_t max_read;
	size_t max_write;
} test_io;

void save_io_test_reset(void) {
	memset(&test_io, 0, sizeof(test_io));
}

void save_io_test_fail(enum SaveIOTestOp op, unsigned call, int error) {
	test_io.op = op;
	test_io.fail_call = call;
	test_io.calls = 0;
	test_io.error = error;
}

void save_io_test_limit_read(size_t bytes) { test_io.max_read = bytes; }
void save_io_test_limit_write(size_t bytes) { test_io.max_write = bytes; }

static int test_should_fail(enum SaveIOTestOp op) {
	if (!test_io.fail_call || test_io.op != op) return 0;
	test_io.calls++;
	if (test_io.calls != test_io.fail_call) return 0;
	errno = test_io.error;
	return 1;
}
#else
#define test_should_fail(op) 0
#endif

static int io_open_file(const char* path, int flags, mode_t mode) {
	if (test_should_fail(SAVE_IO_TEST_OPEN_FILE)) return -1;
	return open(path, flags, mode);
}

static int io_open_dir(const char* path) {
	if (test_should_fail(SAVE_IO_TEST_OPEN_DIR)) return -1;
	return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static ssize_t io_read(int fd, void* data, size_t size) {
#ifdef SAVE_IO_TEST
	if (test_should_fail(SAVE_IO_TEST_READ)) return test_io.error ? -1 : 0;
	if (test_io.max_read && size > test_io.max_read) size = test_io.max_read;
#endif
	return read(fd, data, size);
}

static ssize_t io_write(int fd, const void* data, size_t size) {
#ifdef SAVE_IO_TEST
	if (test_should_fail(SAVE_IO_TEST_WRITE)) return test_io.error ? -1 : 0;
	if (test_io.max_write && size > test_io.max_write) size = test_io.max_write;
#endif
	return write(fd, data, size);
}

static int io_fsync_file(int fd) {
	if (test_should_fail(SAVE_IO_TEST_FSYNC_FILE)) return -1;
	return fsync(fd);
}

static int io_fsync_dir(int fd) {
	if (test_should_fail(SAVE_IO_TEST_FSYNC_DIR)) return -1;
	return fsync(fd);
}

static int io_close_file(int fd) {
	int result = close(fd);
	if (test_should_fail(SAVE_IO_TEST_CLOSE_FILE)) return -1;
	return result;
}

static int io_close_dir(int fd) {
	int result = close(fd);
	if (test_should_fail(SAVE_IO_TEST_CLOSE_DIR)) return -1;
	return result;
}

static int io_rename(const char* old_path, const char* new_path) {
	if (test_should_fail(SAVE_IO_TEST_RENAME)) return -1;
	return rename(old_path, new_path);
}

static int io_unlink(const char* path) {
	if (test_should_fail(SAVE_IO_TEST_UNLINK)) return -1;
	return unlink(path);
}

static int read_all(int fd, void* data, size_t size) {
	unsigned char* out = data;
	size_t done = 0;
	while (done < size) {
		ssize_t got = io_read(fd, out + done, size - done);
		if (got > 0) {
			done += (size_t)got;
			continue;
		}
		if (got < 0 && errno == EINTR) continue;
		if (got == 0) errno = EIO;
		return -1;
	}
	return 0;
}

int save_read_file(const char* path, void* data, size_t capacity,
	int allow_larger, size_t* file_size) {
	if (!path || (!data && capacity)) {
		errno = EINVAL;
		return -1;
	}

	int fd = io_open_file(path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return -1;

	struct stat st;
	if (fstat(fd, &st) || st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) {
		int saved_errno = errno ? errno : EOVERFLOW;
		io_close_file(fd);
		errno = saved_errno;
		return -1;
	}

	size_t actual = (size_t)st.st_size;
	if (!allow_larger && actual > capacity) {
		io_close_file(fd);
		errno = EFBIG;
		return -1;
	}

	size_t read_size = actual < capacity ? actual : capacity;
	if (read_all(fd, data, read_size)) {
		int saved_errno = errno;
		io_close_file(fd);
		errno = saved_errno;
		return -1;
	}

	// A bounded file that grew after fstat must not be mistaken for a valid state.
	if (!allow_larger) {
		unsigned char extra;
		ssize_t got;
		do got = io_read(fd, &extra, 1); while (got < 0 && errno == EINTR);
		if (got != 0) {
			int saved_errno = got < 0 ? errno : EFBIG;
			io_close_file(fd);
			errno = saved_errno;
			return -1;
		}
	}

	if (io_close_file(fd)) return -1;
	if (file_size) *file_size = actual;
	return 0;
}

static int write_all(int fd, const void* data, size_t size) {
	const unsigned char* in = data;
	size_t done = 0;
	while (done < size) {
		ssize_t wrote = io_write(fd, in + done, size - done);
		if (wrote > 0) {
			done += (size_t)wrote;
			continue;
		}
		if (wrote < 0 && errno == EINTR) continue;
		if (wrote == 0) errno = EIO;
		return -1;
	}
	return 0;
}

static int open_parent_dir(const char* path) {
	const char* slash = strrchr(path, '/');
	if (!slash) return io_open_dir(".");

	size_t length = slash == path ? 1 : (size_t)(slash - path);
	char* parent = malloc(length + 1);
	if (!parent) return -1;
	memcpy(parent, path, length);
	parent[length] = '\0';
	int fd = io_open_dir(parent);
	free(parent);
	return fd;
}

int save_write_atomic(const char* path, const void* data, size_t size) {
	if (!path || (!data && size)) {
		errno = EINVAL;
		return -1;
	}

	size_t path_size = strlen(path);
	if (path_size > SIZE_MAX - 5) {
		errno = ENAMETOOLONG;
		return -1;
	}
	char* tmp_path = malloc(path_size + 5);
	if (!tmp_path) return -1;
	memcpy(tmp_path, path, path_size);
	memcpy(tmp_path + path_size, ".tmp", 5);

	int result = -1;
	int fd = io_open_file(tmp_path,
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) goto cleanup;

	if (write_all(fd, data, size) || io_fsync_file(fd)) {
		int saved_errno = errno;
		io_close_file(fd);
		fd = -1;
		errno = saved_errno;
		goto cleanup;
	}
	if (io_close_file(fd)) {
		fd = -1;
		goto cleanup;
	}
	fd = -1;

	int dir_fd = open_parent_dir(path);
	if (dir_fd < 0) goto cleanup;
	if (io_rename(tmp_path, path)) {
		int saved_errno = errno;
		io_close_dir(dir_fd);
		errno = saved_errno;
		goto cleanup;
	}

	// The rename is visible at this point. A failure here means durability could not
	// be confirmed, but the complete new file remains safer than trying to roll back.
	if (io_fsync_dir(dir_fd)) {
		int saved_errno = errno;
		io_close_dir(dir_fd);
		errno = saved_errno;
		free(tmp_path);
		return -1;
	}
	if (io_close_dir(dir_fd)) {
		free(tmp_path);
		return -1;
	}

	result = 0;

cleanup:
	if (fd >= 0) io_close_file(fd);
	if (result) unlink(tmp_path);
	free(tmp_path);
	return result;
}

int save_remove_file(const char* path) {
	if (!path) {
		errno = EINVAL;
		return -1;
	}

	int dir_fd = open_parent_dir(path);
	if (dir_fd < 0) return -1;
	if (io_unlink(path) && errno != ENOENT) {
		int saved_errno = errno;
		io_close_dir(dir_fd);
		errno = saved_errno;
		return -1;
	}
	if (io_fsync_dir(dir_fd)) {
		int saved_errno = errno;
		io_close_dir(dir_fd);
		errno = saved_errno;
		return -1;
	}
	return io_close_dir(dir_fd);
}
