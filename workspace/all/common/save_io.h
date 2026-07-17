#ifndef SAVE_IO_H
#define SAVE_IO_H

#include <stddef.h>

// Read a complete file into data. When allow_larger is true, files larger than
// capacity are accepted and only their prefix is read.
int save_read_file(const char* path, void* data, size_t capacity,
	int allow_larger, size_t* file_size);

// Replace path only after the complete payload has reached stable storage.
int save_write_atomic(const char* path, const void* data, size_t size);

// Remove a file and make the directory update durable. A missing file succeeds.
int save_remove_file(const char* path);

#ifdef SAVE_IO_TEST
enum SaveIOTestOp {
	SAVE_IO_TEST_OPEN_FILE,
	SAVE_IO_TEST_READ,
	SAVE_IO_TEST_WRITE,
	SAVE_IO_TEST_FSYNC_FILE,
	SAVE_IO_TEST_CLOSE_FILE,
	SAVE_IO_TEST_RENAME,
	SAVE_IO_TEST_UNLINK,
	SAVE_IO_TEST_OPEN_DIR,
	SAVE_IO_TEST_FSYNC_DIR,
	SAVE_IO_TEST_CLOSE_DIR,
};

void save_io_test_reset(void);
void save_io_test_fail(enum SaveIOTestOp op, unsigned call, int error);
void save_io_test_limit_read(size_t bytes);
void save_io_test_limit_write(size_t bytes);
#endif

#endif
