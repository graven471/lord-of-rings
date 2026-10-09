#pragma once

#include "macros/core.h"
#include <stddef.h>
#include <stdint.h>
#include <linux/io_uring.h>

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

WARN_UNUSED struct uring_context *uring_context_create(unsigned entries, struct io_uring_params *params);
WARN_UNUSED struct io_uring_sqe *uring_context_get_sqe(struct uring_context *ctx);
int uring_context_submit(struct uring_context *ctx);
WARN_UNUSED struct io_uring_cqe *uring_context_cq_peek(struct uring_context *ctx);
void uring_context_cq_consume(struct uring_context *ctx);
int uring_context_openat(struct uring_context *ctx, const char *path, uint64_t user_data);
int uring_context_read(struct uring_context *ctx, int fd, void *buf, uint32_t len, uint64_t user_data);
int uring_context_write(struct uring_context *ctx, int fd, void *buf, uint32_t len, uint64_t user_data);
void uring_context_destroy(struct uring_context *ctx);
