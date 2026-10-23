#include "getline_compat.h"

#include <stdlib.h>

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) return -1;
    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    size_t len = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (len + 1 >= *n) {
            size_t cap = *n * 2;
            char *p = realloc(*lineptr, cap);
            if (!p) return -1;
            *lineptr = p;
            *n = cap;
        }
        (*lineptr)[len++] = (char)c;
        if (c == '\n') break;
    }
    if (len == 0) return -1;
    (*lineptr)[len] = '\0';
    return (ssize_t)len;
}
