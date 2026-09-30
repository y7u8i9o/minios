#pragma once
/* The scripted TCP peer of netpeer (mode scripted, see scripted.c). */
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>

void scripted_peer(int fd, const struct sockaddr_in *guest, FILE *log,
                   volatile sig_atomic_t *stopping);
