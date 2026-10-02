/* macOS compatibility shim for the glibc-only <error.h> header.
 * The deployment code uses error(status, errnum, fmt, ...) only for fatal
 * argument errors in the keygen tools; glibc semantics approximated. */
#ifndef FLASHORDER_MACOS_ERROR_SHIM
#define FLASHORDER_MACOS_ERROR_SHIM

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error(int status, int errnum, const char *fmt, ...) {
    fflush(stdout);
    fputs("error: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errnum)
        fprintf(stderr, ": %s", strerror(errnum));
    fputc('\n', stderr);
    fflush(stderr);
    if (status)
        exit(status);
}

#endif /* FLASHORDER_MACOS_ERROR_SHIM */
