#pragma once
#include <minios/abi.h>

#define TCSANOW 0
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);
/* Open a pseudo terminal pair: master descriptor and the slave path. */
int openpty(int *master, char *slave_path, size_t size);
