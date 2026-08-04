/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * envspawn -- drive the odls envar directives (SET/ADD/UNSET/PREPEND/
 * APPEND) through a real PMIx_Spawn and print what the spawned process
 * actually ended up with.
 *
 *   envspawn <host>
 *       Spawn "/bin/sh -c <print some vars>" pinned to <host>, carrying one
 *       of every envar directive, and let the child's output flow back
 *       through the IOF.  Waits for the child job to finish, then exits.
 *
 * Why this needs the swarm.  The directives are attached to the job on the
 * HNP, packed into the launch message, and applied by
 * prte_odls_base_process_envars() on whichever daemon actually forks the
 * process.  Pinning the child to a node the parent is NOT on is what makes
 * this a test of the remote daemon's copy rather than the HNP's.
 *
 * And why it exists at all: these directives have no command-line surface -
 * PMIX_SET_ENVAR and friends arrive only through a spawn request - so a
 * PMIx client is the only way to reach them.  ADD in particular is defined
 * as "add envar, but do not overwrite any existing one" (pmix_common.h, and
 * PRTE_JOB_ADD_ENVAR in src/util/attr.h say the same), and it was being
 * applied exactly like SET.  Nothing catches that but asking a real child
 * what its environment looks like.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pmix.h>

/* The child writes one KEY=value line per directive under test into
 * ODLS_REPORT on its OWN node, and the harness reads the file from there.
 * A file rather than stdout on purpose: it takes the IOF entirely out of
 * the picture (a child job's output has its own routing question, which is
 * not what this client is for), and reading it back from the node proves
 * where the fork actually happened.
 *
 * ${VAR-<unset>} distinguishes a variable that was UNSET from one that is
 * merely empty. */
#define ODLS_REPORT "/tmp/odls-envspawn.out"
#define CHILD_SCRIPT                                     \
    "{ echo ODLS_SET=${ODLS_SET-<unset>};"               \
    "  echo ODLS_NEW=${ODLS_NEW-<unset>};"               \
    "  echo ODLS_GONE=${ODLS_GONE-<unset>};"             \
    "  echo ODLS_PATH=${ODLS_PATH-<unset>};"             \
    "  echo ODLS_HOST=$(hostname); } > " ODLS_REPORT

static void load_envar(pmix_info_t *ip, const char *key, const char *name,
                       const char *value)
{
    pmix_envar_t ev;

    PMIX_ENVAR_CONSTRUCT(&ev);
    ev.envar = (char *) name;
    ev.value = (char *) value;
    ev.separator = ':';
    PMIX_INFO_LOAD(ip, key, &ev, PMIX_ENVAR);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    pmix_app_t app;
    pmix_nspace_t child;
    pmix_info_t jobinfo[1];
    const char *host;

    if (2 > argc) {
        fprintf(stderr, "usage: envspawn <host>\n");
        return 2;
    }
    host = argv[1];

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_APP_CONSTRUCT(&app);
    app.cmd = strdup("/bin/sh");
    app.maxprocs = 1;
    PMIx_Argv_append_nosize(&app.argv, "sh");
    PMIx_Argv_append_nosize(&app.argv, "-c");
    PMIx_Argv_append_nosize(&app.argv, CHILD_SCRIPT);

    /* Seven per-app directives.  app.idx is 0, so prte routes every one of
     * them onto the JOB attribute list, in this order - and the order is
     * the semantics: the list is applied front to back, so the ADD of
     * ODLS_SET below lands on a variable the SET before it just created. */
    PMIX_INFO_CREATE(app.info, 7);
    app.ninfo = 7;

    /* pin the child to a named node - that is what puts the fork on a
     * daemon other than the one this parent is talking to */
    PMIX_INFO_LOAD(&app.info[0], PMIX_HOST, host, PMIX_STRING);

    /* SET establishes a value... */
    load_envar(&app.info[1], PMIX_SET_ENVAR, "ODLS_SET", "kept");
    /* ...and ADD must NOT overwrite it.  If ADD is treated as SET, the
     * child reports "clobbered" instead. */
    load_envar(&app.info[2], PMIX_ADD_ENVAR, "ODLS_SET", "clobbered");
    /* ADD on a name nobody has set does establish it */
    load_envar(&app.info[3], PMIX_ADD_ENVAR, "ODLS_NEW", "added");

    /* PREPEND/APPEND edit an existing value in place, using the separator.
     * ODLS_PATH is created by the SET here and then edited twice, which
     * also pins the front-to-back ordering. */
    load_envar(&app.info[4], PMIX_PREPEND_ENVAR, "ODLS_PATH", "front");
    load_envar(&app.info[5], PMIX_APPEND_ENVAR, "ODLS_PATH", "back");

    /* UNSET names a variable and carries no value, so it travels as a
     * STRING rather than an envar - a different branch of the parser and of
     * process_envars, and one that quietly did nothing for a while */
    PMIX_INFO_LOAD(&app.info[6], PMIX_UNSET_ENVAR, "ODLS_GONE", PMIX_STRING);

    /* ODLS_PATH and ODLS_GONE have to exist before the directives above can
     * edit/remove them; put them in the app's own environment so the test
     * does not depend on anything the daemons happen to have inherited. */
    PMIx_Argv_append_nosize(&app.env, "ODLS_PATH=middle");
    PMIx_Argv_append_nosize(&app.env, "ODLS_GONE=present");

    /* the child reports through a file on its own node, so all we ask of
     * the job itself is that it not outlive us */
    PMIX_INFO_LOAD(&jobinfo[0], PMIX_NOTIFY_COMPLETION, NULL, PMIX_BOOL);

    rc = PMIx_Spawn(jobinfo, 1, &app, 1, child);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "ERROR PMIx_Spawn: %s\n", PMIx_Error_string(rc));
        PMIX_APP_DESTRUCT(&app);
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    printf("SPAWNED %s report %s\n", child, ODLS_REPORT);
    fflush(stdout);

    PMIX_APP_DESTRUCT(&app);

    /* give the child time to run and write its report - there is no "wait
     * for job" call available to a plain client */
    sleep(5);

    PMIx_Finalize(NULL, 0);
    return 0;
}
