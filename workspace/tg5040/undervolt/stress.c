// stress — worst-case-ish CPU load for undervolt margin mapping: all cores run a mix of
// NEON fused-multiply-adds (peak datapath draw), integer work, and cache-line churn
// (memcpy) to exercise di/dt. Not a guarantee of worst case (nothing is), which is why
// the campaign keeps a -50mV guardband below the measured cliff.
//   stress <seconds>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <arm_neon.h>

static volatile int run = 1;
static volatile double sink;

static void* worker(void* arg) {
	float32x4_t a = vdupq_n_f32(1.0001f), b = vdupq_n_f32(0.9999f), c = vdupq_n_f32(0.5f);
	unsigned long x = (unsigned long)arg | 1;
	char* buf1 = malloc(65536), *buf2 = malloc(65536);
	if (!buf1 || !buf2) { free(buf1); free(buf2); return (void*)1; }
	memset(buf1, 0xA5, 65536);
	while (run) {
		for (int i = 0; i < 4096; i++) {
			c = vfmaq_f32(c, a, b);
			a = vfmaq_f32(a, b, c);
			b = vfmaq_f32(b, c, a);
			x = x * 6364136223846793005UL + 1442695040888963407UL;
		}
		memcpy(buf2, buf1, 65536);
		buf1[x & 65535] ^= (char)x;
		float r[4]; vst1q_f32(r, c);
		sink += r[0] + (double)(x & 0xff);
	}
	free(buf1); free(buf2);
	return NULL;
}

int main(int argc, char** argv) {
	int secs = (argc > 1) ? atoi(argv[1]) : 60;
	pthread_t t[4];
	int started[4] = {0}, failed = 0;
	// an OK verdict must mean all 4 cores actually stressed: a silently failed spawn or
	// allocation would validate a voltage under partial load (audit 2026-07-12). A nonzero
	// exit is judged CLIFF by uvmap — conservative, never unsafe.
	for (long i = 0; i < 4; i++) {
		if (pthread_create(&t[i], NULL, worker, (void*)i) == 0) started[i] = 1;
		else failed = 1;
	}
	struct timespec ts = { secs, 0 };
	nanosleep(&ts, NULL);
	run = 0;
	for (int i = 0; i < 4; i++) {
		if (!started[i]) continue;
		void* ret = NULL;
		pthread_join(t[i], &ret);
		if (ret) failed = 1;
	}
	if (failed) {
		printf("stress: %ds FAILED (worker spawn/alloc)\n", secs);
		return 2;
	}
	printf("stress: %ds OK (sink %.1f)\n", secs, sink);
	return 0;
}
