#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#define _GNU_SOURCE

#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * lords or ring fandom wiki:

core idea:
 rather than just communicate between kernel and user space with system calls,
 ring buffers are used as the main mode of communication.

 - setup shared buffer with `io_uring_setup(2)` and `mmap(2)`, mapping into user
space shared buffers for submission queue (SQ) and completion queue (CQ) You
place I/O requests you want to make on the SQ, while the kernel places the
results of those operations on the CQ.

 - For every I/O request you need to make (like to read a file, write a file,
accept a socket connection, etc), you create  a submission  queue  entry, or
SQE, describe the I/O operation you need to get done and add it to the tail of
the submission queue (SQ). Each I/O operation is, in essence, the equivalent of
a system call you would have made otherwise, if you were not using io_uring.

  - For instance, a SQE with the opcode set to IORING_OP_READ will request a
read operation to be  issued that is similar to the read(2) system call.

  - After you add one or more SQEs, you need to call io_uring_enter(2) to tell
the kernel to dequeue your I/O requests off the SQ and begin processing them.

  - For each SQE you submit, once it is done processing the request, the kernel
places a completion queue event or CQE at the tail of the completion queue or
CQ.  The kernel places exactly one matching CQE in the CQ for every SQE you
submit on  the SQ. After  you  retrieve a CQE, minimally, you might be
interested in checking the res field of the CQE structure, which corresponds to
the return value of the system call's equivalent, had you used it directly
without using  io_uring.

   - Given that  io_uring  is  an  async interface, errno is never used for
passing back error information. Instead, res will contain what the equivalent
system call would have returned in case of success, and in case of error res
will contain -errno.  For example, if the normal read system call would have
returned -1 and set errno to EINVAL, then res  would  contain  -EINVAL. If the
normal system call would have returned a read size of 1024, then res would
contain 1024.

   - It  is important to remember that I/O requests submitted to the kernel can
complete in any order. It is not necessary for the kernel to process one request
after another, in the order you placed them. Given that the interface is a ring,
the requests are attempted in order, however that doesn't imply  any  sort of
     ordering  on their execution or completion.  When more than one request is
in flight, it is not possible to determine which one will execute or complete
first.  When you dequeue CQEs off the CQ, you should always check which
submitted request it corresponds to. The most common method for doing so is
utilizing the user_data field in the request, which is passed back on the
completion side.

   - You add SQEs to the tail of the SQ.  The kernel reads SQEs off the head of
the queue.
   - The kernel adds CQEs to the tail of the CQ.  You read CQEs off the head of
the queue.

Submission queue polling
     One of the goals of io_uring is to provide a means for efficient I/O.  To
this end, io_uring supports a polling mode that lets you avoid the call  to
io_uring_enter(2),  which  you  use  to  inform the kernel that you have queued
SQEs on to the SQ.  With SQ Polling, io_uring starts a kernel thread that polls
the submission queue for any I/O requests you submit by adding SQEs.  With SQ
Polling enabled, there is no need for you to call io_uring_enter(2), letting you
avoid the  overhead of system calls. A designated kernel thread dequeues SQEs
off the SQ as you add them and dispatches them for asynchronous processing.


 - To submit an I/O request to io_uring, you need to acquire a submission queue
entry (SQE) from the submission queue (SQ), fill it up with details of  the
operation you  want  to submit and call io_uring_enter(2).  There are helper
functions of the form io_uring_prep_X to enable proper setup of the SQE. If you
want to avoid calling io_uring_enter(2), you have the option of setting up
Submission Queue Polling.
 */

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
	const int default_flags = MAP_SHARED | MAP_POPULATE;

	// this is submission queue we will use this to define I/O operations
	// we will write to tail kernel will read head
	// we are producer kernel is consumer
	context->sq_ring = mmap(0, sq_ring_size, default_prot, default_flags, ring_fd, IORING_OFF_SQ_RING);
	// this is completion ring kernel writes to it once it completed a request
	// kernel writes to tail we will read head
	// we are consumer kernel is producer
	context->cq_ring = mmap(0, cq_ring_size, default_prot, default_flags, ring_fd, IORING_OFF_CQ_RING);
	// submission queues entries array we will this up when we want to do some I/O
	context->sqes = mmap(0, sq_entries_size, default_prot, default_flags, ring_fd, IORING_OFF_SQES);

	// cqes = cq_ptr + p.cq_off.cqes;
	context->cqes = (struct io_uring_cqe *)((char *)context->cq_ring + params->cq_off.cqes);

	if (context->sq_ring == MAP_FAILED || context->cq_ring == MAP_FAILED || context->sqes == MAP_FAILED) {
		close(ring_fd);
		perror("mmap");
		return NULL;
	}

	context->sq_ring_mask = RING_PTR(context->sq_ring, params->sq_off.ring_mask);
	context->sq_array = RING_PTR(context->sq_ring, params->sq_off.array);
	context->sq_flags = RING_PTR(context->sq_ring, params->sq_off.flags);

	context->cq_head = RING_PTR(context->cq_ring, params->cq_off.head);
	context->cq_tail = RING_PTR(context->cq_ring, params->cq_off.tail);
	context->cq_ring_mask = RING_PTR(context->cq_ring, params->cq_off.ring_mask);

	return context;
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

	// todo: abstract this into something like uring_context_sumbit
	uint32_t sq_tail = *ring_ctx->sq_tail;
	// sq_ring_mask is usually capacity - 1
	uint32_t index = sq_tail & *ring_ctx->sq_ring_mask;

	struct io_uring_sqe *sqe = &ring_ctx->sqes[index];

	memset(sqe, 0, sizeof(*sqe));

	sqe->opcode = IORING_OP_NOP;
	sqe->user_data = 0x1234;

	ring_ctx->sq_array[index] = index;

	/*
     * everything above this point will become visible to anyone who use store_acquire
     * forms the first half of happens-before
     */
	io_uring_smp_store_release(ring_ctx->sq_tail, sq_tail + 1);

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
}
