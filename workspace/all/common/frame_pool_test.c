#include "frame_pool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_resize;
static void* test_resize(void* ptr, size_t size) {
	if (fail_resize) return NULL;
	return realloc(ptr, size);
}

int main(void) {
	frame_pool_buffer buffers[2];
	memset(buffers, 0, sizeof(buffers));
	frame_pool pool = { buffers, 2, test_resize };

	int a = frame_pool_acquire(&pool, 2, 2, 4);
	int b = frame_pool_acquire(&pool, 2, 2, 4);
	assert(a == 0 && b == 1);
	assert(frame_pool_acquire(&pool, 2, 2, 4) == FRAME_POOL_EXHAUSTED);

	frame_pool_retire(&pool, (size_t)a);
	void* old_px = buffers[a].px;
	size_t old_cap = buffers[a].cap;
	fail_resize = 1;
	assert(frame_pool_acquire(&pool, 8, 8, 16) == FRAME_POOL_OOM);
	assert(buffers[a].px == old_px && buffers[a].cap == old_cap);
	assert(!atomic_load(&buffers[a].busy));
	assert(atomic_load(&buffers[b].busy));

	fail_resize = 0;
	assert(frame_pool_acquire(&pool, 8, 8, 16) == a);
	frame_pool_retire(&pool, (size_t)a);
	frame_pool_retire(&pool, (size_t)b);
	assert(!atomic_load(&buffers[0].busy) && !atomic_load(&buffers[1].busy));
	free(buffers[0].px);
	free(buffers[1].px);
	puts("frame_pool_test: PASS (exhaustion + mid-epoch OOM ownership preserved)");
	return 0;
}
