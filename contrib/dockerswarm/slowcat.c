/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * slowcat -- copy stdin to a file, deliberately slowly.
 *
 * The interesting stdin bugs in IOF are not about volume, they are about
 * back-pressure: they only appear once the application stops keeping up
 * with the daemon that is feeding it.  A normal "cat" drains its stdin
 * pipe as fast as the daemon can fill it, so the daemon's non-blocking
 * write(2) almost always completes in full and the short-write path is
 * never taken.  Real applications are not like that -- the code that
 * exposed issue #2579 was a Fortran solver that copied its stdin to a
 * scratch file before parsing it, one small read at a time -- and once
 * the pipe is saturated every write the daemon attempts is a *partial*
 * write, which is exactly the path that used to re-send the tail of each
 * queued chunk.
 *
 * So this reads its stdin in small bites with a pause between them, keeping
 * the pipe full, and copies every byte it receives to the named file.  The
 * test then compares that file with what was piped in: it must match byte
 * for byte, with nothing lost and -- the #2579 symptom -- nothing
 * duplicated.
 *
 *   slowcat <outfile> [bytes-per-read] [microseconds-between-reads]
 *
 * Deliberately has no PMIx dependency: it says nothing about the runtime,
 * it just consumes stdin the way a slow application does.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLOWCAT_MAX_READ 65536

int main(int argc, char **argv)
{
    char buf[SLOWCAT_MAX_READ];
    size_t nread = 256;
    unsigned long pause_usec = 1000;
    long long total = 0;
    FILE *out;
    ssize_t n;

    if (2 > argc) {
        fprintf(stderr, "usage: %s <outfile> [bytes-per-read] [usec-between-reads]\n", argv[0]);
        return 2;
    }
    if (2 < argc) {
        nread = (size_t) strtoul(argv[2], NULL, 10);
        if (0 == nread || SLOWCAT_MAX_READ < nread) {
            nread = 256;
        }
    }
    if (3 < argc) {
        pause_usec = strtoul(argv[3], NULL, 10);
    }

    out = fopen(argv[1], "w");
    if (NULL == out) {
        fprintf(stderr, "slowcat: cannot open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }

    while (1) {
        n = read(0, buf, nread);
        if (0 > n) {
            if (EINTR == errno || EAGAIN == errno) {
                continue;
            }
            fprintf(stderr, "slowcat: read failed: %s\n", strerror(errno));
            fclose(out);
            return 1;
        }
        if (0 == n) {
            /* EOF -- the runtime delivered the close sentinel */
            break;
        }
        if ((size_t) n != fwrite(buf, 1, (size_t) n, out)) {
            fprintf(stderr, "slowcat: short write to %s: %s\n", argv[1], strerror(errno));
            fclose(out);
            return 1;
        }
        total += n;
        if (0 < pause_usec) {
            usleep(pause_usec);
        }
    }

    if (0 != fclose(out)) {
        fprintf(stderr, "slowcat: close of %s failed: %s\n", argv[1], strerror(errno));
        return 1;
    }
    /* the byte count goes to stdout so the caller can see it without
     * having to trust the file
     */
    fprintf(stdout, "SLOWCAT-BYTES %lld\n", total);
    fflush(stdout);
    return 0;
}
