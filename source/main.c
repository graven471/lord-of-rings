#include "macros/core.h"
#include <asm/unistd_64.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <linux/io_uring.h>
#include <string.h>
#include <unistd.h>
#include "commands/command.h"
#include "commands/cat/cat.h"

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

	// todo: i know O_RDONLY check is not required but some commands like
	// cat etc needs this so i can either two syscall or one i should focus
	// on this later.
	if (access(argv[2], F_OK | O_RDONLY) != 0) {
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
