#ifndef FRAME_POOL_H
#define FRAME_POOL_H

#include <stddef.h>
#include <stdatomic.h>

typedef void* (*frame_pool_resize_fn)(void* ptr, size_t size);

typedef struct {
	void* px;
	size_t cap;
	unsigned w, h;
	size_t pitch;
	_Atomic int busy;
} frame_pool_buffer;

typedef struct {
	frame_pool_buffer* buffers;
	size_t count;
	frame_pool_resize_fn resize;
} frame_pool;

enum { FRAME_POOL_EXHAUSTED = -1, FRAME_POOL_OOM = -2 };

static inline int frame_pool_acquire(frame_pool* pool, unsigned w, unsigned h, size_t pitch) {
	for (size_t i=0; i<pool->count; i++) {
		frame_pool_buffer* b = &pool->buffers[i];
		if (atomic_load_explicit(&b->busy, memory_order_acquire)) continue;
		size_t need = (size_t)h * pitch;
		if (b->cap < need) {
			void* p = pool->resize(b->px, need);
			if (!p) return FRAME_POOL_OOM;
			b->px = p;
			b->cap = need;
		}
		b->w = w;
		b->h = h;
		b->pitch = pitch;
		atomic_store_explicit(&b->busy, 1, memory_order_release);
		return (int)i;
	}
	return FRAME_POOL_EXHAUSTED;
}

static inline void frame_pool_retire(frame_pool* pool, size_t index) {
	if (index < pool->count)
		atomic_store_explicit(&pool->buffers[index].busy, 0, memory_order_release);
}

#endif
