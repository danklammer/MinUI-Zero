#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "save_io.h"

static int failures;
#define CHECK(condition, ...) do { \
	if (!(condition)) { \
		failures++; \
		printf("FAIL: "); \
		printf(__VA_ARGS__); \
		printf(" (%s:%d)\n", __FILE__, __LINE__); \
	} \
} while (0)

static void write_plain(const char* path, const void* data, size_t size) {
	FILE* file = fopen(path, "wb");
	if (!file || fwrite(data, 1, size, file) != size || fclose(file)) {
		perror("write_plain");
		exit(2);
	}
}

static size_t read_plain(const char* path, void* data, size_t capacity) {
	FILE* file = fopen(path, "rb");
	if (!file) return 0;
	size_t size = fread(data, 1, capacity, file);
	fclose(file);
	return size;
}

static void expect_contents(const char* path, const char* expected) {
	char data[64] = {0};
	size_t size = read_plain(path, data, sizeof(data));
	CHECK(size == strlen(expected), "%s size: got %zu, expected %zu",
		path, size, strlen(expected));
	CHECK(!memcmp(data, expected, strlen(expected)), "%s contents changed", path);
}

static void expect_no_tmp(const char* path) {
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	CHECK(access(tmp, F_OK) != 0, "temporary file remains: %s", tmp);
}

static void test_read(const char* path) {
	printf("[read] bounded, prefix-compatible, partial syscalls, EINTR\n");
	write_plain(path, "abcdefgh", 8);

	char data[16] = {0};
	size_t file_size = 0;
	save_io_test_reset();
	save_io_test_limit_read(2);
	CHECK(!save_read_file(path, data, 8, 0, &file_size), "partial reads failed");
	CHECK(file_size == 8 && !memcmp(data, "abcdefgh", 8), "partial read data mismatch");

	memset(data, 0, sizeof(data));
	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_READ, 1, EINTR);
	CHECK(!save_read_file(path, data, 8, 0, &file_size), "EINTR read was not retried");
	CHECK(!memcmp(data, "abcdefgh", 8), "EINTR read data mismatch");

	memset(data, 0, sizeof(data));
	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_READ, 1, 0);
	errno = 0;
	CHECK(save_read_file(path, data, 8, 0, &file_size) < 0 && errno == EIO,
		"premature EOF was not rejected");

	save_io_test_reset();
	errno = 0;
	CHECK(save_read_file(path, data, 4, 0, &file_size) < 0 && errno == EFBIG,
		"oversized bounded read was accepted");

	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_CLOSE_FILE, 1, EIO);
	CHECK(save_read_file(path, data, 8, 0, &file_size) < 0,
		"read-side close failure was accepted");

	memset(data, 0, sizeof(data));
	save_io_test_reset();
	CHECK(!save_read_file(path, data, 4, 1, &file_size), "compatible prefix read failed");
	CHECK(file_size == 8 && !memcmp(data, "abcd", 4), "prefix read mismatch");
}

static void test_write_success(const char* path) {
	printf("[write] complete atomic replace and short-write loop\n");
	write_plain(path, "old", 3);
	save_io_test_reset();
	save_io_test_limit_write(2);
	CHECK(!save_write_atomic(path, "new payload", 11), "atomic write failed: %s", strerror(errno));
	expect_contents(path, "new payload");
	expect_no_tmp(path);

	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_WRITE, 1, EINTR);
	CHECK(!save_write_atomic(path, "after EINTR", 11), "EINTR write was not retried");
	expect_contents(path, "after EINTR");
}

static void test_pre_rename_failures(const char* path) {
	printf("[write] pre-rename failures preserve old data and remove temp\n");
	const enum SaveIOTestOp ops[] = {
		SAVE_IO_TEST_OPEN_FILE,
		SAVE_IO_TEST_WRITE,
		SAVE_IO_TEST_FSYNC_FILE,
		SAVE_IO_TEST_CLOSE_FILE,
		SAVE_IO_TEST_OPEN_DIR,
		SAVE_IO_TEST_RENAME,
	};
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		write_plain(path, "known good", 10);
		save_io_test_reset();
		save_io_test_fail(ops[i], 1, EIO);
		CHECK(save_write_atomic(path, "replacement", 11) < 0,
			"fault %zu unexpectedly succeeded", i);
		expect_contents(path, "known good");
		expect_no_tmp(path);
	}

	write_plain(path, "known good", 10);
	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_WRITE, 1, 0);
	CHECK(save_write_atomic(path, "replacement", 11) < 0,
		"zero-length write unexpectedly succeeded");
	expect_contents(path, "known good");
	expect_no_tmp(path);
}

static void test_post_rename_failure(const char* path) {
	printf("[write] post-rename failures report uncertainty without corrupting data\n");
	const enum SaveIOTestOp ops[] = {
		SAVE_IO_TEST_FSYNC_DIR,
		SAVE_IO_TEST_CLOSE_DIR,
	};
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		write_plain(path, "old", 3);
		save_io_test_reset();
		save_io_test_fail(ops[i], 1, EIO);
		CHECK(save_write_atomic(path, "complete", 8) < 0,
			"post-rename fault %zu succeeded", i);
		expect_contents(path, "complete");
		expect_no_tmp(path);
	}
}

static void test_remove(const char* path) {
	printf("[remove] deletion and directory update are durable\n");
	write_plain(path, "obsolete", 8);
	save_io_test_reset();
	CHECK(!save_remove_file(path), "durable remove failed: %s", strerror(errno));
	CHECK(access(path, F_OK) != 0, "removed file remains");
	CHECK(!save_remove_file(path), "missing file should be a successful remove");

	write_plain(path, "preserve", 8);
	save_io_test_reset();
	save_io_test_fail(SAVE_IO_TEST_UNLINK, 1, EIO);
	CHECK(save_remove_file(path) < 0, "unlink fault unexpectedly succeeded");
	expect_contents(path, "preserve");
}

int main(void) {
	const char* base = getenv("SAVE_IO_TEST_DIR");
	if (!base) base = "/tmp";
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/minui-save-io-XXXXXX", base);
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 2;
	}
	char path[520];
	snprintf(path, sizeof(path), "%s/save", dir);

	test_read(path);
	test_write_success(path);
	test_pre_rename_failures(path);
	test_post_rename_failure(path);
	test_remove(path);

	unlink(path);
	rmdir(dir);
	if (failures) {
		printf("save_io: %d failure(s)\n", failures);
		return 1;
	}
	printf("save_io: all tests passed\n");
	return 0;
}
