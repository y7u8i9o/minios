#pragma once
#include <kernel.h>

struct file;

/* Create the two ends of an anonymous pipe as referenced files. */
int pipe_create(struct file **rd, struct file **wr);
