#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <stdbool.h>

#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

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

/*
 * tries to create uring_context
 *
 * return:
 *
 * NULL -> if anything failed
 * uring_context* -> if succeed
 */
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

	uint64_t tail = *ctx->sq_tail;

	uint64_t index = tail & *ctx->sq_ring_mask;
	ctx->sq_array[index] = index;

	// we are producer so we will write to tail and do smp_release
	io_uring_smp_store_release(ctx->sq_tail, tail + 1);

	return 0;
}

static void uring_context_destroy(struct uring_context *ctx)
{
	if (ctx == NULL)
		return;

	close(ctx->ring_fd);
	free(ctx);
}

int main(void)
{
	printf("io_uring the lord of all rings\n");

	struct io_uring_params params = {
		.flags = IORING_SETUP_SQPOLL,
	};

	struct uring_context *ring_ctx = uring_context_create(32, &params);

	if (ring_ctx == NULL) {
		fprintf(stderr, "failed to create uring context\n");
		exit(1);
	}

	struct io_uring_sqe *op_nop_sqe = uring_context_get_sqe(ring_ctx);

	if (op_nop_sqe == NULL) {
		fprintf(stderr, "failed to get sqe");
		goto error;
	}

	op_nop_sqe->opcode = IORING_OP_NOP;
	op_nop_sqe->user_data = 0x1234;

	if (uring_context_submit(ring_ctx) == -1) {
		fprintf(stderr, "failed to submit to uring");
		goto error;
	}

	struct io_uring_sqe *op_nop_sqe2 = uring_context_get_sqe(ring_ctx);

	if (op_nop_sqe2 == NULL) {
		fprintf(stderr, "failed to get sqe");
		goto error;
	}

	op_nop_sqe2->opcode = IORING_OP_NOP;
	op_nop_sqe2->user_data = 0x5678;

	// todo: how about batch submit ?
	if (uring_context_submit(ring_ctx) == -1) {
		fprintf(stderr, "failed to submit to uring");
		goto error;
	}

	uint32_t sq_flags = io_uring_smp_load_acquire(ring_ctx->sq_flags);

	// if this bit is set then call io_uring_enter() with IORING_ENTER_SQ_WAKEUP to wake the kernel thread
	if (sq_flags & IORING_SQ_NEED_WAKEUP) {
		long ret = syscall(SYS_io_uring_enter, ring_ctx->ring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, NULL, 0);
		printf("wakeup ret=%ld errno=%d\n", ret, errno);
	}

	for (;;) {
		// we don't need any synchronization for head here just need atomicity guarantee
		uint32_t head = io_uring_smp_load_relaxed(ring_ctx->cq_head);
		// this synchronizes with kernel smp_release it does it after writing cqe response
		uint32_t tail = io_uring_smp_load_acquire(ring_ctx->cq_tail);

		// since this is a ring-buffer we know head == tail means ring is empty
		if (head == tail)
			continue;

		uint32_t index = head & *ring_ctx->cq_ring_mask;
		struct io_uring_cqe *cqe = &ring_ctx->cqes[index];

		printf("user_data = 0x%llx\n", cqe->user_data);
		printf("res       = %d\n", cqe->res);
		printf("flags     = %u\n", cqe->flags);

		// kernel acquires this we are saying to kernel we read this response
		// this synchronizes-with kernel's acquire
		io_uring_smp_store_release(ring_ctx->cq_head, head + 1);
	}

	uring_context_destroy(ring_ctx);
	return 0;

error:
	uring_context_destroy(ring_ctx);
	return 1;
}
