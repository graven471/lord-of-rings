#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>
#include <asm/unistd_64.h>
#include <bits/pthreadtypes.h>
#include <errno.h>

#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CPU_RELAX() _mm_pause()

#elif defined(__aarch64__) || defined(__arm__)
#define CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")

#elif defined(__riscv)
#define CPU_RELAX() __asm__ __volatile__("" ::: "memory")

#else
#define CPU_RELAX() __asm__ __volatile__("" ::: "memory")
#endif

#define RING_PTR(base, offset) ((uint32_t *)((char *)(base) + (offset)))

// io_uring(7) man page example
#define io_uring_smp_store_release(p, v) atomic_store_explicit((_Atomic typeof(*(p)) *)(p), (v), memory_order_release)
#define io_uring_smp_load_acquire(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_acquire)
#define io_uring_smp_load_relaxed(p) atomic_load_explicit((_Atomic typeof(*(p)) *)(p), memory_order_relaxed)

struct uring_context {
	int ring_fd;

	void *sq_ring;
	void *cq_ring;
	struct io_uring_sqe *sqes;
	struct io_uring_cqe *cqes;

	uint32_t sq_entries;
	uint32_t cq_entries;

	uint32_t *sq_head;
	uint32_t *sq_tail;
	uint32_t *sq_ring_mask;
	uint32_t *sq_array;
	uint32_t *sq_flags;

	uint32_t *cq_head;
	uint32_t *cq_tail;
	uint32_t *cq_ring_mask;
};

static struct uring_context *uring_context_create(unsigned entries, struct io_uring_params *params)
{
	if (entries == 0 || params == NULL)
		return NULL;

	long ring_fd = syscall(SYS_io_uring_setup, entries, params);

	if (ring_fd < 0) {
		perror("syscall");
		return NULL;
	}

	const size_t sq_ring_size = params->sq_off.array + params->sq_entries * sizeof(uint32_t);
	const size_t cq_ring_size = params->cq_off.cqes + params->cq_entries * sizeof(struct io_uring_cqe);
	const size_t sq_entries_size = params->sq_entries * sizeof(struct io_uring_sqe);
	const size_t cq_entries_size = params->cq_entries * sizeof(struct io_uring_cqe);

	assert(sq_ring_size > 0 || cq_ring_size > 0 || sq_entries_size > 0 || cq_entries_size > 0);

	struct uring_context *context = malloc(sizeof(struct uring_context));

	if (context == NULL) {
		close(ring_fd);
		return NULL;
	}

	context->ring_fd = ring_fd;

	// TODO: handle this
	if (params->features & IORING_FEAT_SINGLE_MMAP) {
		// in this case we can combine SQ and CQ ring into one mmap
		// instead of two conceptually
		// [ mapping start ]
		// sq_off.head, sq_off.tail, sq_off.ring_mask ...  ← SQ control
		// ...
		// cq_off.head, cq_off.tail, cq_off.ring_mask ...  ← CQ control
		// cq_off.cqes ...                                  ← CQE structs live here
		// too [ mapping end ]
	}

	const int default_prot = PROT_READ | PROT_WRITE;
	const int default_flag = MAP_SHARED | MAP_POPULATE;

	// this is submission queue we will use this to define I/O operations
	// we will write to tail kernel will read head
	// we are producer kernel is consumer
	context->sq_ring = mmap(0, sq_ring_size, default_prot, default_flag, ring_fd, IORING_OFF_SQ_RING);
	// this is completion ring kernel writes to it once it completed a request
	// kernel writes to tail we will read head
	// we are consumer kernel is producer
	context->cq_ring = mmap(0, cq_ring_size, default_prot, default_flag, ring_fd, IORING_OFF_CQ_RING);
	// submission queues entries array we will this up when we want to do some I/O
	context->sqes = mmap(0, sq_entries_size, default_prot, default_flag, ring_fd, IORING_OFF_SQES);

	// cqes = cq_ptr + p.cq_off.cqes;
	context->cqes = (struct io_uring_cqe *)((char *)context->cq_ring + params->cq_off.cqes);

	if (context->sq_ring == MAP_FAILED || context->cq_ring == MAP_FAILED || context->sqes == MAP_FAILED) {
		close(ring_fd);
		perror("mmap");
		return NULL;
	}

	context->sq_entries = params->sq_entries;
	context->cq_entries = params->cq_entries;

	context->sq_head = RING_PTR(context->sq_ring, params->sq_off.head);
	context->sq_tail = RING_PTR(context->sq_ring, params->sq_off.tail);
	context->sq_ring_mask = RING_PTR(context->sq_ring, params->sq_off.ring_mask);
	context->sq_array = RING_PTR(context->sq_ring, params->sq_off.array);
	context->sq_flags = RING_PTR(context->sq_ring, params->sq_off.flags);

	context->cq_head = RING_PTR(context->cq_ring, params->cq_off.head);
	context->cq_tail = RING_PTR(context->cq_ring, params->cq_off.tail);
	context->cq_ring_mask = RING_PTR(context->cq_ring, params->cq_off.ring_mask);

	return context;
}

static inline bool is_ring_free(uint32_t size, uint32_t head, uint32_t tail)
{
	// todo: can this under or overflow ?
	return size - (tail - head) == 0 ? false : true;
}

static struct io_uring_sqe *uring_context_get_sqe(struct uring_context *ctx)
{
	if (ctx == NULL)
		return NULL;

	uint32_t head = *ctx->sq_head;
	uint32_t tail = *ctx->sq_tail;

	// todo: handle this instead of returning null
	if (!is_ring_free(ctx->sq_entries, head, tail)) {
		return NULL;
	}

	uint32_t index = tail & *ctx->sq_ring_mask;
	struct io_uring_sqe *sqe = &ctx->sqes[index];

	memset(sqe, 0, sizeof(*sqe));
	// conceptually it means at this position in the submission queue,
	// which SQE should the kernel consume ?
	ctx->sq_array[index] = index;

	return sqe;
}

static int uring_context_submit(struct uring_context *ctx)
{
	if (ctx == NULL)
		return -1;

	uint32_t tail = io_uring_smp_load_acquire(ctx->sq_tail);
	uint64_t index = tail & *ctx->sq_ring_mask;

	// we are producer so we will write to tail and do smp_release
	io_uring_smp_store_release(ctx->sq_tail, tail + 1);

	return 0;
}

static struct io_uring_cqe *uring_context_cq_peek(struct uring_context *ctx)
{
	if (ctx == NULL)
		return NULL;

	uint32_t sq_flags = io_uring_smp_load_acquire(ctx->sq_flags);

	// if this bit is set then call io_uring_enter() with IORING_ENTER_SQ_WAKEUP to wake the kernel thread
	if (sq_flags & IORING_SQ_NEED_WAKEUP) {
		if (syscall(__NR_io_uring_enter, ctx->ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, NULL, 0) != 0) {
			perror("syscall");
			return NULL;
		}
	}

	// here kernel updates the tail of cq_ring when it writes response and it did release
	// so we have to do acquire
	uint32_t cq_tail = io_uring_smp_load_acquire(ctx->cq_tail);

	// we don't need any synchronization for head because kernel does not touches it
	// only us userspace update it kernel just reads it
	// we just need to preserve atomicity
	uint32_t cq_head = io_uring_smp_load_relaxed(ctx->cq_head);

	// since this is a ring buffer we know head == tail means buffer is empty
	// todo: instead of returning null should i wait for response ?
	if (cq_head == cq_tail) {
		return NULL;
	}

	unsigned index = cq_head & *ctx->cq_ring_mask;
	struct io_uring_cqe *cqe = &ctx->cqes[index];

	return cqe;
}

static void uring_context_cq_consume(struct uring_context *ctx)
{
	uint32_t cq_head = io_uring_smp_load_relaxed(ctx->cq_head);
	// we are saying the kernel hey we have read this response
	// kernel is doing acquire on cq_head
	io_uring_smp_store_release(ctx->cq_head, cq_head + 1);
}

static void uring_context_destroy(struct uring_context *ctx)
{
	if (ctx == NULL)
		return;

	// todo: should i unmap rings ?

	close(ctx->ring_fd);
	free(ctx);
}

static atomic_uint event_count = 0;

// obselete for now
static void *uring_context_check_event(void *args)
{
	int ring_fd = (int)(intptr_t)args;
	fprintf(stderr, "worker started, ring_fd=%d\n", ring_fd);

	// for now wait for one event to get completed
	long res = syscall(__NR_io_uring_enter, ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
	fprintf(stderr, "io_uring_enter returned: %ld, errno: %s\n", res, strerror(errno));

	if (res < 0)
		return (void *)(intptr_t)-1;

	// this thread will do release and main thread or any consuming thread
	// will do acquire so this make sures that anything before release will be flushed
	atomic_fetch_add_explicit(&event_count, 1, memory_order_release);
	fprintf(stderr, "worker set event_count\n");

	return NULL;
}

static int uring_context_openat(struct uring_context *ctx, const char *path, uint64_t user_data)
{
	struct io_uring_sqe *sqe = uring_context_get_sqe(ctx);

	if (sqe == NULL)
		return -1;

	sqe->opcode = IORING_OP_OPENAT;
	sqe->fd = AT_FDCWD;
	sqe->addr = (uintptr_t)path;
	// todo: support flags
	sqe->open_flags = O_RDONLY | O_CLOEXEC;
	sqe->user_data = user_data;

	if (uring_context_submit(ctx) == -1)
		return -1;

	return 0;
}
static int uring_context_read(struct uring_context *ctx, int fd, void *buf, size_t len, uint64_t user_data)
{
	// get a sqe, submit a read op, set user_data to user_data

	if (fd < 0)
		return -1;

	struct io_uring_sqe *sqe = uring_context_get_sqe(ctx);

	if (sqe == NULL)
		return -1;

	sqe->opcode = IORING_OP_READ;
	sqe->fd = fd;
	sqe->user_data = user_data;
	sqe->flags = 0;
	sqe->off = 0;
	sqe->addr = (uintptr_t)buf;
	sqe->len = len;

	if (uring_context_submit(ctx) == -1)
		return -1;

	return 0;
}

static int uring_context_write(struct uring_context *ctx, int fd, void *buf, size_t len, uint64_t user_data)
{
	struct io_uring_sqe *sqe = uring_context_get_sqe(ctx);

	if (sqe == NULL)
		return -1;

	sqe->opcode = IORING_OP_WRITE;
	sqe->fd = fd;
	sqe->addr = (uintptr_t)buf;
	sqe->len = len;
	sqe->user_data = user_data;

	if (uring_context_submit(ctx) == -1)
		return -1;

	return 0;
}

int main(void)
{
	printf("io_uring the lord of all rings\n");

	struct io_uring_params params = {
		.flags = IORING_SETUP_SQPOLL,
		// todo: just a luck number for now change this later
		// based on workload
		.sq_thread_idle = 500,
	};

	struct uring_context *ring_ctx = uring_context_create(32, &params);

	if (ring_ctx == NULL) {
		fprintf(stderr, "failed to create uring context\n");
		exit(1);
	}

	if (uring_context_openat(ring_ctx, "/etc/os-release", 0x01) == -1) {
		fprintf(stderr, "failed to do openat\n");
		goto error;
	}

	char buf[4096];
	bool done = false;
	for (;;) {
		// todo: replace 1000 with better algorithm
		for (int i = 0; i < 1000; ++i) {
			struct io_uring_cqe *cqe = uring_context_cq_peek(ring_ctx);

			if (cqe == NULL) {
				CPU_RELAX();
				continue;
			}

			switch (cqe->user_data) {
			case 0x01: {
				uring_context_read(ring_ctx, cqe->res, buf, 4096, 0x02);
				uring_context_cq_consume(ring_ctx);
				break;
			}
			case 0x02: {
				if (cqe->res < 0) {
					fprintf(stderr, "failed to read\n");
					goto error;
				}

				if (uring_context_write(ring_ctx, STDOUT_FILENO, buf, cqe->res, 0x03) == -1) {
					fprintf(stderr, "failed to write\n");
					goto error;
				}

				uring_context_cq_consume(ring_ctx);
				done = true;
				break;
			}
			case 0x03:
				uring_context_cq_consume(ring_ctx);
				break;

			default:
				break;
			}

			if (done)
				break;
		}

		syscall(__NR_io_uring_enter, ring_ctx->ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
		continue;
	}

	uring_context_destroy(ring_ctx);
	return 0;

error:
	uring_context_destroy(ring_ctx);
	return 1;
}
