/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_open_config_file(): a file whose contents decide how this process
 * behaves is read only if nobody but root or the user we run as could
 * have put it there. The bootstrap cases in test_ras cover ownership,
 * modes, links and the directory through prte_bootstrap_parse(); these
 * are the answers that only the helper itself gives: what it says, or
 * does not say, about a file it could not use.
 */

#include "prte_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/util/pmix_printf.h"
#include "src/util/prte_config_file.h"

int test_config_file(void);

#define CHECK(label, cond)                                                 \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL [config_file:%s]: %s\n", label, #cond);  \
            failures++;                                                    \
        }                                                                  \
    } while (0)

int test_config_file(void)
{
    int failures = 0;
    char dir[] = "/tmp/prte_config_file_XXXXXX";
    char *good = NULL, *fifo = NULL, *missing = NULL;
    const char *why = "unset";
    FILE *fp;

    if (NULL == mkdtemp(dir)) {
        fprintf(stderr, "FAIL [config_file]: mkdtemp\n");
        return 1;
    }
    pmix_asprintf(&good, "%s/good", dir);
    pmix_asprintf(&fifo, "%s/fifo", dir);
    pmix_asprintf(&missing, "%s/missing", dir);

    fp = fopen(good, "w");
    if (NULL != fp) {
        fprintf(fp, "x\n");
        fclose(fp);
    }
    chmod(good, 0644);
    fp = prte_open_config_file(good, &why);
    CHECK("a file of ours, only we can write, is read", NULL != fp && NULL == why);
    if (NULL != fp) {
        fclose(fp);
    }

    /* not there: no test failed, so the caller says "not found" */
    why = "unset";
    fp = prte_open_config_file(missing, &why);
    CHECK("a missing file gives no reason", NULL == fp && NULL == why && ENOENT == errno);

    /* a FIFO would block the open, and is not a file of anything */
    why = NULL;
    CHECK("a FIFO is refused, at once",
          0 == mkfifo(fifo, 0600) && NULL == prte_open_config_file(fifo, &why) && NULL != why);

    why = NULL;
    CHECK("a directory is refused", NULL == prte_open_config_file(dir, &why) && NULL != why);

    unlink(good);
    unlink(fifo);
    rmdir(dir);
    free(good);
    free(fifo);
    free(missing);
    if (0 == failures) {
        fprintf(stdout, "PASSED test_config_file\n");
    }
    return failures;
}
