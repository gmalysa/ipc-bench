/**
 * Small benchmark program to measure the cost of different IPC mechanisms
 *
 * By default some of these will only work on multicore machines because they
 * employ userspace spinlocks.
 *
 * Add -D SUPPORT_SINGLE_CORE in order to insert sched_yield(), which has a small
 * static cost on multicore machines, in order to provide a poor quality userspace
 * wait
 */

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// compare against appropriate __atomic gcc intrinsics
#define READ_ONCE(p) (*((volatile typeof(p) *)(&p)))
#define WRITE_ONCE(p, v) (*((volatile typeof(p) *)&p) = v)

#define DEFAULT_ITERATIONS 1000
#define DEFAULT_SIZE 4096
static uint64_t iterations = DEFAULT_ITERATIONS;
static uint64_t sz = DEFAULT_SIZE;

#define NUM_ITEMS (sz / sizeof(uint8_t))

static char *folder = ".";

uint64_t get_ts(struct timespec *ts) {
	return ts->tv_sec * 1000000000 + ts->tv_nsec;
}

void dump_times(const char *label, struct timespec *times, uint8_t extra) {
	size_t count;
	char *fname;
	int res;

	res = asprintf(&fname, "%s/%s_%d-%d.txt", folder, label, getpid(), extra);
	(void)res;

	FILE *fp = fopen(fname, "wb");
	for (count = 0; count < iterations; ++count) {
		fprintf(fp, "%zu,", get_ts(&times[count+1]) - get_ts(&times[count]));
	}
	fclose(fp);
	free(fname);

	char *avgstr;

	double avg = (get_ts(&times[iterations]) - get_ts(&times[0]))*1./(iterations);
	res = asprintf(&avgstr, "avg %f ns", avg);
	(void)res;

	double acc = 0.0;
	for (size_t i = 1; i < iterations+1; ++i) {
		double delta = get_ts(&times[i]) - get_ts(&times[i-1]);
		double incr = delta - avg;
		acc += incr*incr;
	}

	char *varstr;
	// note: sample variance / n-1
	res = asprintf(&varstr, "stddev %f ns", sqrt(acc / (iterations-1)));
	(void)res;
	printf("%s, %s\n", avgstr, varstr);

	free(varstr);
	free(avgstr);
}

/**
 * The first entry in the buffer is always skipped because in the shm version
 * it is used for a shared spinlock
 */
void process_buffer(uint8_t *buf, uint8_t check, uint8_t new) {
	size_t i;

	for (i = 1; i < NUM_ITEMS; ++i) {
		if (buf[i] != check) {
			printf("buffer failure in position %zu\n", i);
			exit(1);
		}
		buf[i] = new;
	}
}

void process_sockpair(uint8_t *buf, int fd, uint8_t check, uint8_t new) {
	recv(fd, buf, sz, 0);
	process_buffer(buf, check, new);
	send(fd, buf, sz, 0);
}

uint8_t process_shm(uint8_t *buf, uint8_t check, uint8_t new) {
	size_t i;

	while (READ_ONCE(buf[0]) != check) {
#ifdef SUPPORT_SINGLE_CORE
		sched_yield();
#endif
	}

	process_buffer(buf, check, new);
	WRITE_ONCE(buf[0], new);
}

uint8_t process_sema(uint8_t *buf, sem_t *in, sem_t *out, uint8_t check,
	uint8_t new)
{
	size_t i;

	sem_wait(in);
	process_buffer(buf, check, new);
	sem_post(out);
}

void run_shm_ipc_test(void) {
	pid_t pid;
	uint8_t *buf;
	uint8_t id;
	size_t count;
	struct timespec *times;

	buf = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (!buf)
		perror("mmap failure");

	memset(buf, 0, sz*sizeof(*buf));

	pid = fork();
	printf("process id %d\n", getpid());

	if (0 == pid)
		id = 1;
	else
		id = 0;

	process_shm(buf, id, 1-id);

	times = calloc(iterations+1, sizeof(*times));
	if (!times) {
		printf("calloc failure in %d, kill the other process too\n", getpid());
		exit(1);
	}

	count = 0;
	while (count < iterations+1) {
		clock_gettime(CLOCK_MONOTONIC, &times[count]);
		process_shm(buf, id, 1-id);
		count += 1;
	}

	dump_times("shm_ipc", times, 0);
	free(times);
	munmap(buf, sz);
}

void run_sockpair_ipc_test(void) {
	pid_t pid;
	int sv[2];
	int fd;
	uint8_t check, new;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		perror("failed to create socket pair");
		exit(1);
	}

	pid = fork();
	if (0 == pid) {
		fd = sv[0];
		check = 1;
		new = 0;
	}
	else {
		fd = sv[1];
		check = 0;
		new = 1;
	}

	uint8_t *buf = calloc(1, sz*sizeof(*buf));
	if (!buf) {
		printf("calloc failure in %d, kill the other process too\n", getpid());
		exit(1);
	}

	struct timespec *times = calloc(iterations+1, sizeof(*times));
	if (!times) {
		printf("calloc failure in %d, kill the other process too\n", getpid());
		exit(1);
	}

	if (0 == pid) {
		send(fd, buf, sz, 0);
	}

	size_t count;
	while (count < iterations+1) {
		clock_gettime(CLOCK_MONOTONIC, &times[count]);
		process_sockpair(buf, fd, check, new);
		count += 1;
	}

	dump_times("sockpair_ipc", times, 0);
	free(times);
	free(buf);
	close(fd);
}

struct thread_info {
	uint8_t *buf;
	uint8_t id;
	pthread_t tid;
	sem_t *in;
	sem_t *out;
};

void *shm_thread(void *arg) {
	size_t count;
	struct timespec *times;
	struct thread_info *info = arg;
	uint8_t *buf = info->buf;
	uint8_t id = info->id;

	times = calloc(iterations+1, sizeof(*times));
	if (!times) {
		printf("calloc failure in thread\n");
		exit(1);
	}

	// sync thread start points
	process_shm(buf, id, 1-id);

	count = 0;
	while (count < iterations+1) {
		clock_gettime(CLOCK_MONOTONIC, &times[count]);
		process_shm(buf, id, 1-id);
		count += 1;
	}

	dump_times("shm_thread_spinlock", times, id);
	free(times);
	return NULL;
}

void run_shm_thread_test(void) {
	uint8_t *buf = calloc(1, sz*sizeof(*buf));
	struct thread_info me = {
		.buf = buf,
		.id = 0,
	};
	struct thread_info you = {
		.buf = buf,
		.id = 1,
	};
	void *result;

	if (!buf) {
		printf("buf allocation failed\n");
		exit(1);
	}

	pthread_create(&me.tid, NULL, shm_thread, &me);
	pthread_create(&you.tid, NULL, shm_thread, &you);

	pthread_join(me.tid, &result);
	pthread_join(you.tid, &result);
}

void *shm_thread_sema(void *arg) {
	size_t count;
	struct timespec *times;
	struct thread_info *info = arg;
	uint8_t *buf = info->buf;
	uint8_t id = info->id;
	sem_t *in = info->in;
	sem_t *out = info->out;

	times = calloc(iterations+1, sizeof(*times));
	if (!times) {
		printf("calloc failure in thread\n");
		exit(1);
	}

	process_sema(buf, in, out, id, 1-id);

	count = 0;
	while (count < iterations+1) {
		clock_gettime(CLOCK_MONOTONIC, &times[count]);
		process_sema(buf, in, out, id, 1-id);
		count += 1;
	}

	dump_times("shm_thread_sema", times, id);
	free(times);
	return NULL;
}

void run_shm_thread_sema_test(void) {
	uint8_t *buf = calloc(1, sz*sizeof(*buf));
	sem_t sem[2];
	struct thread_info me = {
		.buf = buf,
		.id = 0,
		.in = &sem[0],
		.out = &sem[1],
	};
	struct thread_info you = {
		.buf = buf,
		.id = 1,
		.in = &sem[1],
		.out = &sem[0],
	};
	void *result;

	if (!buf) {
		printf("buf allocation failed\n");
		exit(1);
	}

	sem_init(&sem[0], 0, 1);
	sem_init(&sem[1], 0, 0);

	pthread_create(&me.tid, NULL, shm_thread_sema, &me);
	pthread_create(&you.tid, NULL, shm_thread_sema, &you);

	pthread_join(me.tid, &result);
	pthread_join(you.tid, &result);
}

void usage(void) {
	printf("\n");
	printf("  ./ipc-bench [opts]\n");
	printf("\n");
	printf(" -f [name]         Folder prefix for output, default .\n");
	printf(" -m [mode]         Run specific test mode, see below\n");
	printf(" -i [num]          Repeat the test for [num] iterations\n");
	printf(" -n [num]          Operate on [num] bytes in the buffer\n");
	printf("\n");
	printf("  Possible values for mode:\n");
	printf("    m - shared memory ipc test between two processes (atomic counter)\n");
	printf("    p - sockpair ipc test between two processes (send/recv)\n");
	printf("    t - shared memory test between two threads (atomic counter)\n");
	printf("    x - shared memory test between two threads (semaphore)\n");
	printf("\n");
	exit(1);
}

int main(int argc, char **argv) {
	char mode = 'm';
	int opt;

	while ((opt = getopt(argc, argv, "f:m:i:n:h")) != -1) {
		switch (opt) {
		case 'f':
			folder = optarg;
			break;
		case 'm':
			mode = optarg[0];
			break;
		case 'i':
			iterations = strtoul(optarg, NULL, 0);
			printf("using %lu iterations\n", iterations);
			break;
		case 'n':
			sz = strtoul(optarg, NULL, 0);
			printf("using %lu alloc size\n", sz);
			break;
		default:
			printf("invalid argument: %c\n", opt);
			/* fallthrough */
		case 'h':
			usage();
			break;
		}
	}

	printf("note that these numbers are provided for convenience but you need\n");
	printf("to look at a histogram of the csv data produced in the output file\n");
	printf("to understand why they are not meaningful.\n");
	printf("hint: the data is multi-modal\n");

	switch (mode) {
	case 'm':
		run_shm_ipc_test();
		break;
	case 'p':
		run_sockpair_ipc_test();
		break;
	case 't':
		run_shm_thread_test();
		break;
	case 'x':
		run_shm_thread_sema_test();
		break;
	default:
		printf("invalid mode selected please try again\n");
	}
}
