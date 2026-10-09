#pragma once

struct command {
	const char *name;
	int (*handler)(int argc, char **argv);
};

