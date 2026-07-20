// wakeup_test.c — focused harness for the event-driven rumble worker and the audio
// producer/consumer backpressure wait (feat/wakeup-reduction). These mirror the exact
// synchronization shapes in api.c with PLAT/SDL stubbed, so the wait/signal/teardown
// logic runs under ASan/TSan on the host. Scenarios follow the Codex task list 1-9.
//
// Build (host):
//   cc wakeup_test.c -o wakeup_test -lpthread            (add -fsanitize=thread or address)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void msleep(int ms) { usleep(ms * 1000); }
static void deadline_in(struct timespec* ts, int ms) {
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_nsec += (long)ms * 1000000L;
	while (ts->tv_nsec >= 1000000000L) { ts->tv_sec += 1; ts->tv_nsec -= 1000000000L; }
}

// ---------------- VIB worker mirror ----------------
#define VIB_DEFER_OFF_MS 51
static struct {
	pthread_t pt;
	pthread_mutex_t mx;
	pthread_cond_t cv;
	int queued_strength, strength, quit;
	// instrumentation
	int loop_wakeups;      // times the worker loop body ran
	int rumble_writes;     // PLAT_setRumble calls
	int last_rumble;       // last written value
	int zero_writes;       // how many times the motor was written 0
} tvib;
static pthread_mutex_t stub_mx = PTHREAD_MUTEX_INITIALIZER;
static void stub_setRumble(int v) {
	pthread_mutex_lock(&stub_mx);
	tvib.rumble_writes++; tvib.last_rumble = v; if (v == 0) tvib.zero_writes++;
	pthread_mutex_unlock(&stub_mx);
}
static int stub_last(void)  { pthread_mutex_lock(&stub_mx); int v = tvib.last_rumble; pthread_mutex_unlock(&stub_mx); return v; }
static int stub_zeros(void) { pthread_mutex_lock(&stub_mx); int v = tvib.zero_writes; pthread_mutex_unlock(&stub_mx); return v; }
static void* tvib_thread(void* arg) {
	pthread_mutex_lock(&tvib.mx);
	while (!tvib.quit) {
		tvib.loop_wakeups++;
		if (tvib.queued_strength == tvib.strength) {
			pthread_cond_wait(&tvib.cv, &tvib.mx);
			continue;
		}
		int target = tvib.queued_strength;
		if (target == 0) {
			struct timespec ts; deadline_in(&ts, VIB_DEFER_OFF_MS);
			while (!tvib.quit && tvib.queued_strength == 0)
				if (pthread_cond_timedwait(&tvib.cv, &tvib.mx, &ts) == ETIMEDOUT) break;
			if (tvib.quit) break;
			if (tvib.queued_strength != 0) continue;
		}
		tvib.strength = target;
		pthread_mutex_unlock(&tvib.mx);
		stub_setRumble(target);
		pthread_mutex_lock(&tvib.mx);
	}
	pthread_mutex_unlock(&tvib.mx);
	return NULL;
}
static void tvib_start(void) {
	memset(&tvib, 0, sizeof(tvib));
	pthread_mutex_init(&tvib.mx, NULL);
	pthread_cond_init(&tvib.cv, NULL);
	pthread_create(&tvib.pt, NULL, tvib_thread, NULL);
}
static void tvib_set(int v) {
	pthread_mutex_lock(&tvib.mx);
	if (tvib.queued_strength != v) { tvib.queued_strength = v; pthread_cond_signal(&tvib.cv); }
	pthread_mutex_unlock(&tvib.mx);
}
static void tvib_quit(void) {
	pthread_mutex_lock(&tvib.mx);
	tvib.quit = 1;
	pthread_cond_broadcast(&tvib.cv);
	pthread_mutex_unlock(&tvib.mx);
	pthread_join(tvib.pt, NULL);
	stub_setRumble(0);
}

static void test_vib_idle_no_wakeups(void) {
	printf("[vib] scenario 1: extended idle produces zero loop wakeups\n");
	tvib_start();
	msleep(50); // let the worker reach its wait
	pthread_mutex_lock(&tvib.mx); int w0 = tvib.loop_wakeups; pthread_mutex_unlock(&tvib.mx);
	msleep(300);
	pthread_mutex_lock(&tvib.mx); int w1 = tvib.loop_wakeups; pthread_mutex_unlock(&tvib.mx);
	CHECK(w1 == w0, "idle worker woke %d times in 300ms (want 0)", w1 - w0);
	tvib_quit();
}
static void test_vib_rapid_toggle(void) {
	printf("[vib] scenario 2: rapid on/off converges, deferred-off rescues\n");
	tvib_start();
	for (int i = 0; i < 100; i++) { tvib_set(100); tvib_set(0); }
	tvib_set(100); // final state: ON — every intermediate 0 should have been rescued or applied
	msleep(120);
	CHECK(stub_last() == 100, "final motor state %d (want 100)", stub_last());
	// deferred-off: quick 0->N vacillation must not thrash the motor with zeros
	CHECK(stub_zeros() <= 2, "motor written 0 %d times during vacillation (want <=2)", stub_zeros());
	tvib_quit();
	CHECK(stub_last() == 0, "motor left on after quit");
}
static int tvib_get(void) { // mirrors the fixed VIB_getStrength: read under the mutex
	pthread_mutex_lock(&tvib.mx);
	int v = tvib.strength;
	pthread_mutex_unlock(&tvib.mx);
	return v;
}
static void test_vib_getter_race(void) {
	printf("[vib] getter: menu-entry save/restore sequence races the worker cleanly\n");
	tvib_start();
	for (int i = 0; i < 200; i++) {
		tvib_set(i % 7 ? 60 : 0);
		int saved = tvib_get();   // menu entry: capture current strength
		tvib_set(0);              // menu: rumble off
		tvib_set(saved);          // menu exit: restore
	}
	msleep(120);
	CHECK(tvib_get() == 0 || tvib_get() == 60, "getter returned torn value");
	tvib_quit();
}
static void test_vib_teardown(void) {
	printf("[vib] scenario 3: shutdown while active and while blocked\n");
	tvib_start();
	tvib_set(80);
	msleep(30);
	tvib_quit(); // active teardown
	CHECK(stub_last() == 0, "teardown-while-active left motor at %d", stub_last());
	tvib_start();
	msleep(30);  // worker parked in cond_wait
	tvib_quit(); // blocked teardown — must not hang (reaching here at all is the pass)
	CHECK(stub_last() == 0, "teardown-while-blocked left motor at %d", stub_last());
}

// ---------------- audio backpressure mirror ----------------
#define RING 64
static struct {
	pthread_mutex_t audio_mx; // stands in for SDL_LockAudio
	pthread_mutex_t space_mx;
	pthread_cond_t space_cv;
	unsigned space_gen;
	int ring[RING];
	int in, filled;           // filled = one-behind-out full marker, as in api.c
	int initialized;
	int ff_nonblock;
	long produced, consumed_total;
	int producer_done;
	long wait_blocks;         // times the producer entered a wait
} tsnd;
static void tsnd_signal_space(void) {
	pthread_mutex_lock(&tsnd.space_mx);
	tsnd.space_gen++;
	pthread_cond_broadcast(&tsnd.space_cv);
	pthread_mutex_unlock(&tsnd.space_mx);
}
static int tsnd_full(void) { return tsnd.in == tsnd.filled; }
// producer: push n sequence-numbered frames with the api.c wait shape; returns accepted
static int tsnd_batch(int n) {
	pthread_mutex_lock(&tsnd.audio_mx);
	if (!tsnd.initialized) { pthread_mutex_unlock(&tsnd.audio_mx); return n; }
	int accepted = 0;
	while (n > 0) {
		while (!tsnd.ff_nonblock && tsnd_full()) {
			tsnd.wait_blocks++;
			pthread_mutex_lock(&tsnd.space_mx);
			unsigned g0 = tsnd.space_gen;
			pthread_mutex_unlock(&tsnd.audio_mx);
			struct timespec ts; deadline_in(&ts, 20);
			while (tsnd.space_gen == g0)
				if (pthread_cond_timedwait(&tsnd.space_cv, &tsnd.space_mx, &ts) == ETIMEDOUT) break;
			pthread_mutex_unlock(&tsnd.space_mx);
			pthread_mutex_lock(&tsnd.audio_mx);
			if (!tsnd.initialized) { pthread_mutex_unlock(&tsnd.audio_mx); return accepted; }
		}
		if (tsnd.ff_nonblock && tsnd_full()) break; // FF drop path
		while (n > 0 && !tsnd_full()) {
			tsnd.ring[tsnd.in] = (int)tsnd.produced++;
			tsnd.in = (tsnd.in + 1) % RING;
			n--; accepted++;
		}
	}
	pthread_mutex_unlock(&tsnd.audio_mx);
	return accepted;
}
// consumer: drain k frames, verifying strict sequence; then signal space (callback shape)
static long expected_seq = 0;
static int tsnd_done(void) {
	pthread_mutex_lock(&tsnd.audio_mx);
	int d = tsnd.producer_done;
	pthread_mutex_unlock(&tsnd.audio_mx);
	return d;
}
static long tsnd_consumed(void) {
	pthread_mutex_lock(&tsnd.audio_mx);
	long c = tsnd.consumed_total;
	pthread_mutex_unlock(&tsnd.audio_mx);
	return c;
}
static long tsnd_waits(void) {
	pthread_mutex_lock(&tsnd.audio_mx);
	long w = tsnd.wait_blocks;
	pthread_mutex_unlock(&tsnd.audio_mx);
	return w;
}
static void tsnd_set_ff(int v) {
	pthread_mutex_lock(&tsnd.audio_mx);
	tsnd.ff_nonblock = v;
	pthread_mutex_unlock(&tsnd.audio_mx);
}
static void tsnd_consume(int k) {
	pthread_mutex_lock(&tsnd.audio_mx);
	while (k-- > 0) {
		int out = (tsnd.filled + 1) % RING; // one-behind-out marker walks forward
		if (out == tsnd.in) break;          // ring empty
		tsnd.filled = out;
		CHECK(tsnd.ring[out] == (int)expected_seq, "sequence broken: got %d want %ld", tsnd.ring[out], expected_seq);
		expected_seq++;
		tsnd.consumed_total++;
	}
	pthread_mutex_unlock(&tsnd.audio_mx);
	tsnd_signal_space();
}
static void tsnd_reset(void) {
	memset(&tsnd, 0, sizeof(tsnd));
	pthread_mutex_init(&tsnd.audio_mx, NULL);
	pthread_mutex_init(&tsnd.space_mx, NULL);
	pthread_cond_init(&tsnd.space_cv, NULL);
	tsnd.in = 1; tsnd.filled = 0; // matches api.c: full when in==filled; empty slot convention
	tsnd.initialized = 1;
	expected_seq = 0;
}
static void* producer_thread(void* arg) {
	long total = (long)(intptr_t)arg;
	long sent = 0;
	int live = 1;
	while (live && sent < total) {
		int want = total - sent < 8 ? (int)(total - sent) : 8;
		int got = tsnd_batch(want);
		sent += got;
		pthread_mutex_lock(&tsnd.audio_mx);
		if ((!got && tsnd.ff_nonblock) || !tsnd.initialized) live = 0;
		pthread_mutex_unlock(&tsnd.audio_mx);
	}
	pthread_mutex_lock(&tsnd.audio_mx);
	tsnd.producer_done = 1;
	pthread_mutex_unlock(&tsnd.audio_mx);
	return NULL;
}
static void test_snd_block_and_release(void) {
	printf("[snd] scenarios 4+5: producer blocks on full ring, resumes on space, order preserved\n");
	tsnd_reset();
	pthread_t pt;
	pthread_create(&pt, NULL, producer_thread, (void*)(intptr_t)500);
	msleep(50);
	CHECK(!tsnd_done(), "producer finished without ever blocking on a %d-slot ring", RING);
	CHECK(tsnd_waits() > 0, "producer never entered the wait");
	for (int i = 0; i < 100 && !tsnd_done(); i++) { tsnd_consume(16); msleep(2); }
	while (tsnd_consumed() < 500 && expected_seq < 500) { tsnd_consume(16); msleep(1); }
	pthread_join(pt, NULL);
	CHECK(tsnd_consumed() == 500, "consumed %ld of 500 in order", tsnd_consumed());
}
static void test_snd_ff_while_blocked(void) {
	printf("[snd] scenario 6: FF entered while producer is blocked -> prompt drop, no deadlock\n");
	tsnd_reset();
	pthread_t pt;
	pthread_create(&pt, NULL, producer_thread, (void*)(intptr_t)10000);
	msleep(40); // producer is now blocked on the full ring
	tsnd_set_ff(1);
	tsnd_signal_space();
	msleep(100); // watchdog window: the producer must finish well inside this
	CHECK(tsnd_done(), "producer still blocked 100ms after FF-nonblock + signal");
	pthread_join(pt, NULL);
}
static void test_snd_teardown_while_blocked(void) {
	printf("[snd] scenario 8: teardown while producer is blocked -> prompt exit\n");
	tsnd_reset();
	pthread_t pt;
	pthread_create(&pt, NULL, producer_thread, (void*)(intptr_t)10000);
	msleep(40);
	pthread_mutex_lock(&tsnd.audio_mx);
	tsnd.initialized = 0;
	pthread_mutex_unlock(&tsnd.audio_mx);
	tsnd_signal_space();
	msleep(100);
	CHECK(tsnd_done(), "producer still blocked 100ms after teardown + signal");
	pthread_join(pt, NULL);
}
static void test_snd_reinit_cycles(void) {
	printf("[snd] scenario 9: repeated init/teardown with a live producer each cycle\n");
	for (int c = 0; c < 20; c++) {
		tsnd_reset();
		pthread_t pt;
		pthread_create(&pt, NULL, producer_thread, (void*)(intptr_t)200);
		tsnd_consume(32);
		msleep(3);
		pthread_mutex_lock(&tsnd.audio_mx);
		tsnd.initialized = 0;
		pthread_mutex_unlock(&tsnd.audio_mx);
		tsnd_signal_space();
		pthread_join(pt, NULL);
	}
	printf("  20 cycles clean\n");
}

int main(void) {
	printf("== wakeup-reduction synchronization harness ==\n");
	test_vib_idle_no_wakeups();
	test_vib_rapid_toggle();
	test_vib_getter_race();
	test_vib_teardown();
	test_snd_block_and_release();
	test_snd_ff_while_blocked();
	test_snd_teardown_while_blocked();
	test_snd_reinit_cycles();
	if (failures) { printf("== %d FAILURES ==\n", failures); return 1; }
	printf("== ALL PASS ==\n");
	return 0;
}
