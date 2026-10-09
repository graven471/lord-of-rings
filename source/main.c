#include "macros/core.h"
#include "uring/uring.h"
#include <asm/unistd_64.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/io_uring.h>
#include <string.h>
#include <unistd.h>
#include "commands/command.h"

int cmd_cat(int argc, char **argv)
{
	printf("i am cat\n");
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
		uring_context_destroy(ring_ctx);
		exit(1);
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
					uring_context_destroy(ring_ctx);
					exit(1);
				}

				if (uring_context_write(ring_ctx, STDOUT_FILENO, buf, cqe->res, 0x03) == -1) {
					fprintf(stderr, "failed to write\n");
					uring_context_destroy(ring_ctx);
					exit(1);
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
}

int cmd_cp(int argc, char **argv)
{
	printf("i am %s, %s\n", argv[0], argv[1]);
	return 0;
}

int cmd_mv(int argc, char **argv)
{
	printf("i am %s, %s\n", argv[0], argv[1]);
	return 0;
}

static const struct command commands[] = { { "cat", cmd_cat }, { "cp", cmd_cp }, { "mv", cmd_mv } };

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "Usage: lords_of_the_rings <command> <args...>\n");
		fprintf(stderr, "Commands: cat, cp, mv\n");
		exit(1);
	}

	if (access(argv[2], F_OK) != 0) {
		perror("access");
		return -1;
	}

	for (size_t i = 0; i < ARRAY_SIZE(commands); ++i) {
		if (strncmp(argv[1], commands[i].name, 50) == 0) {
			return commands[i].handler(argc - 1, argv + 1);
		}
	}

	fprintf(stderr, "Unknown command: %s\n", argv[1]);
	return 2;
}
