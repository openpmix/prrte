/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * prte_tm_spawn - start a DVM's daemons as tasks of the PBS job, through
 * PBS's TM API.
 *
 * plm/tm runs this on the HNP's node; nobody else should, which is why it
 * is installed in libexecdir rather than bindir. It exists so that PRRTE
 * never loads the TM library: some builds of it carry private copies of
 * libraries PMIx is also linked against, and the two cannot share a
 * process. This program links the TM library and the C library and
 * nothing of PRRTE's or PMIx's - prte_config.h is included for its
 * definitions only.
 *
 *     prte_tm_spawn <vpid-index> <daemon argv...>
 *
 * Every daemon is started with the same argv, except that argument
 * <vpid-index> (counted from the daemon's argv[0]) becomes its vpid.
 *
 * Environment: the daemons' environment, which also carries the PBS_*
 * variables tm_init() reads. The mom hands a task exactly the
 * environment tm_spawn() is given and searches its PATH for argv[0].
 *
 * Standard input, written by plm/tm and then closed:
 *
 *     NAME=value      zero or more lines, added to every daemon's
 *     ...             environment and to nothing else - the DVM key is
 *                     one, and must be neither on a command line nor in
 *                     this program's own environment
 *     <empty line>
 *     <vpid> <host>   one line per daemon
 *     ...
 *
 * Standard output, one line per event:
 *
 *     <vpid> no-such-host 0     <host> is not a node of the job
 *     <vpid> spawn-failed <e>   TM refused or failed the spawn (TM error e)
 *     <vpid> exited <status>    the daemon's task has ended
 *
 * Every daemon gets exactly one such line, unless TM itself fails: then
 * the reason goes to stderr and we exit 1, and plm/tm takes every daemon
 * without a line to be lost with us. Otherwise we exit 0 once every
 * daemon has had its line - that is, once every task we started has
 * ended - so we live as long as the DVM does. Our tasks do not depend on
 * us: PBS keeps them, and charges the job for them, after we have gone.
 */

#include "prte_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if PRTE_PLM_TM_HAVE_TM
#    include <tm.h>
#else
#    include "testbuild_tm.h"
#endif

extern char **environ;

typedef struct {
    unsigned long vpid;
    char *host;
    tm_task_id tid;
    tm_event_t spawn_ev;
    tm_event_t obit_ev;
    int obitval;
    int spawned; /* its spawn event has arrived */
    int done;    /* it has had its line */
} daemon_t;

static void burn(char *s)
{
    if (NULL != s) {
        volatile char *p = s;
        while ('\0' != *p) {
            *p++ = '\0';
        }
    }
}

static void report(const daemon_t *d, const char *what, int value)
{
    printf("%lu %s %d\n", d->vpid, what, value);
    fflush(stdout);
}

/* the name a line of PBS_NODEFILE gives, as pbs_tmrsh reads it */
static void chomp(char *line)
{
    size_t n = strlen(line);
    while (0 < n && isspace((unsigned char) line[n - 1])) {
        line[--n] = '\0';
    }
}

/* Which TM node <host> is. Node i of tm_nodeinfo() is line i of
 * PBS_NODEFILE - the mapping pbs_tmrsh relies on - so it is the first
 * line naming the host. A name the HNP shortened from the file's FQDN
 * matches that name's first label. -1 if none does. */
static int find_node(char **lines, int nlines, const char *host)
{
    size_t hlen = strlen(host);
    int i;

    for (i = 0; i < nlines; i++) {
        if (0 == strcmp(lines[i], host)) {
            return i;
        }
    }
    if (NULL == strchr(host, '.')) {
        for (i = 0; i < nlines; i++) {
            if (0 == strncmp(lines[i], host, hlen) && '.' == lines[i][hlen]) {
                return i;
            }
        }
    }
    return -1;
}

static int read_nodefile(char ***lines_out, int *nlines_out)
{
    const char *path = getenv("PBS_NODEFILE");
    char **lines = NULL, *line = NULL, **tmp;
    size_t cap = 0;
    int nlines = 0;
    FILE *fp;

    if (NULL == path) {
        fprintf(stderr, "prte_tm_spawn: PBS_NODEFILE is not set\n");
        return -1;
    }
    if (NULL == (fp = fopen(path, "r"))) {
        fprintf(stderr, "prte_tm_spawn: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (-1 != getline(&line, &cap, fp)) {
        chomp(line);
        if (NULL == (tmp = realloc(lines, (nlines + 1) * sizeof(char *)))) {
            fclose(fp);
            return -1;
        }
        lines = tmp;
        if (NULL == (lines[nlines] = strdup(line))) {
            fclose(fp);
            return -1;
        }
        nlines++;
    }
    free(line);
    fclose(fp);
    *lines_out = lines;
    *nlines_out = nlines;
    return 0;
}

/* The environment each daemon gets: ours, with the additions from stdin
 * replacing any variable of the same name. The additions are not copied:
 * the caller wipes them. */
static char **task_env(char **extra, int nextra)
{
    char **env;
    int n = 0, i, j, k = 0;

    while (NULL != environ[n]) {
        n++;
    }
    if (NULL == (env = calloc(n + nextra + 1, sizeof(char *)))) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        size_t len = strcspn(environ[i], "=");
        for (j = 0; j < nextra; j++) {
            if (0 == strncmp(environ[i], extra[j], len) && '=' == extra[j][len]) {
                break;
            }
        }
        if (j == nextra) {
            env[k++] = environ[i];
        }
    }
    for (j = 0; j < nextra; j++) {
        env[k++] = extra[j];
    }
    env[k] = NULL;
    return env;
}

static int read_input(char ***extra_out, int *nextra_out, daemon_t **daemons_out,
                      int *ndaemons_out)
{
    char *line = NULL, **extra = NULL, **xtmp, *host, *end;
    daemon_t *daemons = NULL, *dtmp;
    int nextra = 0, ndaemons = 0, in_env = 1, rc = -1;
    size_t cap = 0;
    unsigned long vpid;

    while (-1 != getline(&line, &cap, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (in_env) {
            if ('\0' == line[0]) {
                in_env = 0;
                continue;
            }
            if (NULL == strchr(line, '=')) {
                fprintf(stderr, "prte_tm_spawn: environment input is not NAME=value\n");
                goto out;
            }
            if (NULL == (xtmp = realloc(extra, (nextra + 1) * sizeof(char *)))) {
                goto out;
            }
            extra = xtmp;
            if (NULL == (extra[nextra] = strdup(line))) {
                goto out;
            }
            nextra++;
            burn(line);
            continue;
        }
        errno = 0;
        vpid = strtoul(line, &end, 10);
        if (0 != errno || end == line || ' ' != *end || '\0' == end[1]) {
            fprintf(stderr, "prte_tm_spawn: bad daemon line \"%s\"\n", line);
            goto out;
        }
        host = end + 1;
        if (NULL == (dtmp = realloc(daemons, (ndaemons + 1) * sizeof(daemon_t)))) {
            goto out;
        }
        daemons = dtmp;
        memset(&daemons[ndaemons], 0, sizeof(daemon_t));
        daemons[ndaemons].vpid = vpid;
        if (NULL == (daemons[ndaemons].host = strdup(host))) {
            goto out;
        }
        ndaemons++;
    }
    if (in_env) {
        fprintf(stderr, "prte_tm_spawn: input ended before the daemon list\n");
        goto out;
    }
    rc = 0;

out:
    burn(line);
    free(line);
    *extra_out = extra;
    *nextra_out = nextra;
    *daemons_out = daemons;
    *ndaemons_out = ndaemons;
    return rc;
}

int main(int argc, char **argv)
{
    struct tm_roots roots;
    tm_node_id *nodes = NULL;
    tm_event_t ev;
    char **extra = NULL, **env = NULL, **dargv, **lines = NULL, vpid[32];
    daemon_t *daemons = NULL;
    int nextra = 0, ndaemons = 0, nnodes = 0, nlines = 0;
    int vpid_index, dargc, pending = 0, rc, err, i, idx;
    char *end;

    if (3 > argc) {
        fprintf(stderr, "prte_tm_spawn is run by PRRTE, not by hand\n");
        return 1;
    }
    errno = 0;
    vpid_index = (int) strtol(argv[1], &end, 10);
    dargv = &argv[2];
    dargc = argc - 2;
    if (0 != errno || '\0' != *end || 0 >= vpid_index || dargc <= vpid_index) {
        fprintf(stderr, "prte_tm_spawn: bad vpid index \"%s\"\n", argv[1]);
        return 1;
    }

    rc = read_input(&extra, &nextra, &daemons, &ndaemons);
    if (0 != rc) {
        goto fail;
    }
    if (NULL == (env = task_env(extra, nextra))) {
        goto fail;
    }
    if (0 != read_nodefile(&lines, &nlines)) {
        goto fail;
    }

    if (TM_SUCCESS != (rc = tm_init(NULL, &roots))) {
        fprintf(stderr, "prte_tm_spawn: tm_init failed: TM error %d\n", rc);
        goto fail;
    }
    if (TM_SUCCESS != (rc = tm_nodeinfo(&nodes, &nnodes))) {
        fprintf(stderr, "prte_tm_spawn: tm_nodeinfo failed: TM error %d\n", rc);
        goto fail;
    }

    /* every spawn at once: the mother superior's mom fans them out */
    for (i = 0; i < ndaemons; i++) {
        idx = find_node(lines, nlines, daemons[i].host);
        if (0 > idx || nnodes <= idx) {
            report(&daemons[i], "no-such-host", 0);
            daemons[i].done = 1;
            continue;
        }
        snprintf(vpid, sizeof(vpid), "%lu", daemons[i].vpid);
        dargv[vpid_index] = vpid;
        /* the tid is written when the spawn's event arrives, so it must
         * stay where it is until then */
        rc = tm_spawn(dargc, dargv, env, nodes[idx], &daemons[i].tid, &daemons[i].spawn_ev);
        if (TM_SUCCESS != rc) {
            report(&daemons[i], "spawn-failed", rc);
            daemons[i].done = 1;
            continue;
        }
        pending++;
    }
    /* tm_spawn sent each environment as it went; the key is no longer
     * needed here */
    for (i = 0; i < nextra; i++) {
        burn(extra[i]);
    }

    /* each daemon has a spawn event and, once spawned, an obit event */
    while (0 < pending) {
        rc = tm_poll(TM_NULL_EVENT, &ev, 1, &err);
        if (TM_SUCCESS != rc) {
            fprintf(stderr, "prte_tm_spawn: tm_poll failed: TM error %d\n", rc);
            goto fail;
        }
        for (i = 0; i < ndaemons; i++) {
            if (daemons[i].done) {
                continue;
            }
            if (!daemons[i].spawned && ev == daemons[i].spawn_ev) {
                daemons[i].spawned = 1;
                if (TM_SUCCESS != err) {
                    report(&daemons[i], "spawn-failed", err);
                    daemons[i].done = 1;
                    pending--;
                } else if (TM_SUCCESS != (rc = tm_obit(daemons[i].tid, &daemons[i].obitval,
                                                       &daemons[i].obit_ev))) {
                    /* started, but its end cannot be watched; the HNP
                     * still hears of a daemon that has reported and
                     * then dies, from its connection */
                    fprintf(stderr, "prte_tm_spawn: tm_obit failed for daemon %lu: TM error %d\n",
                            daemons[i].vpid, rc);
                    daemons[i].done = 1;
                    pending--;
                }
                break;
            }
            if (daemons[i].spawned && ev == daemons[i].obit_ev) {
                report(&daemons[i], "exited", (TM_SUCCESS == err) ? daemons[i].obitval : -1);
                daemons[i].done = 1;
                pending--;
                break;
            }
        }
    }
    (void) tm_finalize();
    return 0;

fail:
    for (i = 0; i < nextra; i++) {
        burn(extra[i]);
    }
    return 1;
}
