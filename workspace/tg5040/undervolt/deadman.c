// deadman — starvation-resistant dead-man switch for calibration stress rounds.
//   deadman <budget_seconds>
// If not killed (SIGTERM/SIGINT) before the budget expires, force an immediate reboot
// via the reboot(2) syscall — no init, no shutdown scripts, no sync at trigger time
// (a wedged storage stack must not be able to hang the reboot this exists to guarantee;
// the campaign syncs its breadcrumb BEFORE arming us).
//
// Why a compiled tool instead of `sleep && reboot -f`: the 2026-07-12 campaigns froze
// at marginal voltages in a state where normal-priority userspace never ran again while
// procd (RT) kept feeding the hardware watchdog. SCHED_FIFO max priority + mlockall
// gives this process cycles whenever the scheduler gives anything cycles, and the raw
// syscall avoids exec/library work at trigger time. A wedge deeper than the scheduler
// is covered by the panic_on_rcu_stall net uvmap arms alongside us.
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t cancelled = 0;
static void on_term(int sig) { (void)sig; cancelled = 1; }

int main(int argc, char** argv) {
	int budget = (argc > 1) ? atoi(argv[1]) : 0;
	if (budget <= 0) { fprintf(stderr, "usage: deadman <budget_seconds>\n"); return 2; }

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	struct sched_param sp;
	memset(&sp, 0, sizeof(sp));
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0)
		fprintf(stderr, "deadman: SCHED_FIFO unavailable, running normal priority\n");
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fprintf(stderr, "deadman: mlockall failed\n");

	// sync while the system is healthy — never at trigger time
	sync();

	struct timespec t0, now;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		if (cancelled) return 0;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - t0.tv_sec >= budget) break;
		struct timespec tick = { 0, 250 * 1000 * 1000 };
		nanosleep(&tick, NULL);
	}

	fprintf(stderr, "deadman: budget expired — forcing reboot\n");
	reboot(RB_AUTOBOOT);
	// reboot(2) only returns on failure; last resort: sysrq emergency reboot
	int fd = open("/proc/sysrq-trigger", O_WRONLY);
	if (fd >= 0) { ssize_t w = write(fd, "b", 1); (void)w; close(fd); }
	return 1;
}
