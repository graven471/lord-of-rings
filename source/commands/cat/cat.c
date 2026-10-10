#include "cat.h"
#include "macros/core.h"
#include "uring/uring.h"
#include <asm/unistd_64.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <linux/io_uring.h>
#include <unistd.h>

#define CHUNK_SIZE (64 * 1024)
#define INFLIGHT_MAX 16

#define UD_READ(slot) ((uint64_t)(0x02) << 32 | (slot))
#define UD_WRITE(slot) ((uint64_t)(0x03) << 32 | (slot))
#define UD_OPENAT(slot) ((uint64_t)(0x01) << 32 | (slot))

#define UD_TYPE(ud) ((ud) >> 32)
#define UD_SLOT(ud) ((ud) & 0xFFFFFFFF)

struct allocation {
	void *buf;
	long size;
	size_t cursor;
};

static inline struct allocation *allocate_bytes(const char *filename)
{
	struct stat st = { 0 };

	if (unlikely(stat(filename, &st) != 0)) {
		perror("stat");
		return NULL;
	}

	struct allocation *alloc = malloc(sizeof(struct allocation));

	if (unlikely(alloc == NULL)) {
		perror("malloc");
		return NULL;
	}

	alloc->size = st.st_size;
	alloc->cursor = 0;
	alloc->buf = malloc((size_t)alloc->size);

	if (alloc->buf == NULL) {
		perror("malloc");
		return NULL;
	}

	return alloc;
}

static inline int cat_openat(struct uring_context *ctx, const char *filename)
{
	if (uring_context_openat(ctx, filename, UD_OPENAT(0x01)) != 0)
		return -1;

	return uring_context_submit(ctx, 1);
}

static inline int cat_read(struct uring_context *ctx, struct io_uring_cqe *cqe, struct allocation *alloc)
{
	if (cqe->res < 0)
		return -1;

	int fd = cqe->res;

	// consume the openat cqe
	uring_context_cq_consume(ctx);

	for (size_t i = 0; i < INFLIGHT_MAX; i++) {
		uint64_t offset = i * (uint64_t)CHUNK_SIZE;
		uint32_t len = CHUNK_SIZE;

		if (offset + (uint64_t)CHUNK_SIZE > (uint64_t)alloc->size) {
			len = MIN((uint32_t)CHUNK_SIZE, (uint32_t)alloc->size - (uint32_t)offset);
		}

		if (unlikely(uring_context_read(ctx, fd, (char *)alloc->buf + offset, len, offset, UD_READ(i)) != 0))
			return -1;

		printf("offset = %lu, len = %d\n", offset, len);
	}

	return uring_context_submit(ctx, INFLIGHT_MAX);
}

static inline int cat_write(struct uring_context *ctx, struct io_uring_cqe *cqe, struct allocation *alloc)
{
	if (cqe->res < 0) {
		perror("read failed\n");
		return -1;
	}

	uint32_t type = UD_TYPE(cqe->user_data);
	uint32_t slot = UD_SLOT(cqe->user_data);

	// consume read
	uring_context_cq_consume(ctx);

	for (int i = 0; i < INFLIGHT_MAX; i++) {
		printf("cq_head = %d, cq_tail = %d\n", *ctx->cq_head, *ctx->cq_tail);

		// todo:
		// handle it in order e.g. if we get slot = 2 first then slot = 1
		// then store slot = 2 in somewhere wait for slot = 1
		// then write

		cqe = uring_context_cq_peek(ctx);
		type = UD_TYPE(cqe->user_data);
		slot = UD_SLOT(cqe->user_data);

		printf("type = %d, slot = %d\n", type, slot);
		uring_context_cq_consume(ctx);
	}

	return 0;
}

int cmd_cat(UNUSED int argc, char **argv)
{
	const char *filename = argv[1];

	struct io_uring_params uring_params = {
		.flags = IORING_SETUP_SQPOLL,
		.sq_thread_idle = 500,

	};

	struct uring_context *ctx = uring_context_create(32, &uring_params);

	if (unlikely(ctx == NULL)) {
		exit(1);
	}

	struct allocation *alloc = allocate_bytes(filename);

	if (unlikely(alloc == NULL)) {
		uring_context_destroy(ctx);
		exit(1);
	}

	if (cat_openat(ctx, filename) == -1)
		goto error;

	bool done = false;
	for (;;) {
		struct io_uring_cqe *cqe = uring_context_cq_peek(ctx);

		if (unlikely(cqe == NULL)) {
			goto error;
		}

		switch (UD_TYPE(cqe->user_data)) {
		case 0x01: {
			if (unlikely(cat_read(ctx, cqe, alloc) != 0)) {
				fprintf(stderr, "failed to read file contents\n");
				goto error;
			}

			break;
		}

		case 0x02: {
			if (unlikely(cat_write(ctx, cqe, alloc) != 0)) {
				fprintf(stderr, "failed to write to stdout\n");
				goto error;
			}

			break;
		}

		case 0x03: {
			uring_context_cq_consume(ctx);
			done = true;
			break;
		}

		default:
			break;
		}

		if (likely(done) == true)
			break;
	}

	uring_context_destroy(ctx);
	free(alloc);

	return 0;

error:
	uring_context_destroy(ctx);
	free(alloc);
	exit(1);
}
