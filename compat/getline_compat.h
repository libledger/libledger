/* POSIX getline() for platforms that lack it (MinGW / Windows).
 * Force-included by the Makefile on Windows; the sources stay untouched. */
#ifndef GETLINE_COMPAT_H
#define GETLINE_COMPAT_H

#include <stdio.h>
#include <sys/types.h>

ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#endif /* GETLINE_COMPAT_H */
