/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_CMD_LINE_H
#define PRTE_CMD_LINE_H

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/util/pmix_argv.h"

BEGIN_C_DECLS

/* define the command line options that PRRTE internally understands.
 * It is the responsibility of each schizo component to translate its
 * command line inputs to these definitions. The definitions are provided
 * to help avoid errors due to typos - i.e., where the schizo component
 * interprets its CLI but assigns it to an erroneous string */

//      NAME                            STRING                      ARGUMENT

// Basic options
#define PRTE_CLI_HELP                   "help"                      // optional
#define PRTE_CLI_VERSION                "version"                   // none
#define PRTE_CLI_VERBOSE                "verbose"                   // number of instances => verbosity level
#define PRTE_CLI_PARSEABLE              "parseable"                 // none
#define PRTE_CLI_PARSABLE               "parsable"                  // none
#define PRTE_CLI_PERSONALITY            "personality"               // required

// MCA parameter options
#define PRTE_CLI_PRTEMCA                "prtemca"                   // requires TWO
#define PRTE_CLI_PMIXMCA                "pmixmca"                   // requires TWO
#define PRTE_CLI_TUNE                   "tune"                      // required

// DVM options
#define PRTE_CLI_NO_READY_MSG           "no-ready-msg"              // none
#define PRTE_CLI_DAEMONIZE              "daemonize"                 // none
#define PRTE_CLI_SYSTEM_SERVER          "system-server"             // none
#define PRTE_CLI_SET_SID                "set-sid"                   // none
#define PRTE_CLI_REPORT_PID             "report-pid"                // required
#define PRTE_CLI_REPORT_URI             "report-uri"                // required
#define PRTE_CLI_TEST_SUICIDE           "test-suicide"              // none
#define PRTE_CLI_DEFAULT_HOSTFILE       "default-hostfile"          // required
#define PRTE_CLI_SINGLETON              "singleton"                 // required
#define PRTE_CLI_KEEPALIVE              "keepalive"                 // required
#define PRTE_CLI_LAUNCH_AGENT           "launch-agent"              // required
#define PRTE_CLI_MAX_VM_SIZE            "max-vm-size"               // required
#define PRTE_CLI_DEBUG                  "debug"                     // none
#define PRTE_CLI_DEBUG_DAEMONS          "debug-daemons"             // none
#define PRTE_CLI_DEBUG_DAEMONS_FILE     "debug-daemons-file"        // none
#define PRTE_CLI_LEAVE_SESSION_ATTACHED "leave-session-attached"    // none
#define PRTE_CLI_TMPDIR                 "tmpdir"                    // required
#define PRTE_CLI_PREFIX                 "prefix"                    // required
#define PRTE_CLI_NOPREFIX               "noprefix"                  // none
#define PRTE_CLI_FWD_SIGNALS            "forward-signals"           // required
#define PRTE_CLI_RUN_AS_ROOT            "allow-run-as-root"         // none
#define PRTE_CLI_STREAM_BUF             "stream-buffering"          // required

// Application options
#define PRTE_CLI_NP                     "np"                        // required
#define PRTE_CLI_NPERNODE               "N"                         // required
#define PRTE_CLI_APPFILE                "app"                       // required
#define PRTE_CLI_FWD_ENVAR              "x"                         // required
#define PRTE_CLI_HOSTFILE               "hostfile"                  // required
#define PRTE_CLI_HOST                   "host"                      // required
#define PRTE_CLI_PATH                   "path"                      // required
#define PRTE_CLI_PSET                   "pset"                      // required
#define PRTE_CLI_PRELOAD_FILES          "preload-files"             // required
#define PRTE_CLI_PRELOAD_BIN            "preload-binary"            // none
#define PRTE_CLI_STDIN                  "stdin"                     // required
#define PRTE_CLI_OUTPUT                 "output"                    // required
#define PRTE_CLI_WDIR                   "wdir"                      // required
#define PRTE_CLI_SET_CWD_SESSION        "set-cwd-to-session-dir"    // none
#define PRTE_CLI_ENABLE_RECOVERY        "enable-recovery"           // none
#define PRTE_CLI_DISABLE_RECOVERY       "disable-recovery"          // none

// Placement options
#define PRTE_CLI_MAPBY                  "map-by"                    // required
#define PRTE_CLI_RANKBY                 "rank-by"                   // required
#define PRTE_CLI_BINDTO                 "bind-to"                   // required

// Runtime options
#define PRTE_CLI_RTOS                   "runtime-options"           // required

// Debug options
#define PRTE_CLI_DO_NOT_LAUNCH          "do-not-launch"             // none
#define PRTE_CLI_DISPLAY                "display"                   // required
#define PRTE_CLI_XTERM                  "xterm"                     // none
#define PRTE_CLI_DO_NOT_AGG_HELP        "no-aggregate-help"         // none

// Tool connection options
#define PRTE_CLI_SYS_SERVER_FIRST       "system-server-first"       // none
#define PRTE_CLI_SYS_SERVER_ONLY        "system-server-only"        // none
#define PRTE_CLI_DO_NOT_CONNECT         "do-not-connect"            // none
#define PRTE_CLI_WAIT_TO_CONNECT        "wait-to-connect"           // required
#define PRTE_CLI_NUM_CONNECT_RETRIES    "num-connect-retries"       // required
#define PRTE_CLI_PID                    "pid"                       // required
#define PRTE_CLI_NAMESPACE              "namespace"                 // required
#define PRTE_CLI_DVM_URI                "dvm-uri"                   // required

// Daemon-specific CLI options
#define PRTE_CLI_PUBSUB_SERVER          "prte-server"               // required
#define PRTE_CLI_CONTROLLER_URI         "dvm-master-uri"            // required
#define PRTE_CLI_PARENT_URI             "parent-uri"                // required
#define PRTE_CLI_TREE_SPAWN             "tree-spawn"                // required
#define PRTE_CLI_PLM                    "plm"                       // required


/* define accepted synonyms - these must be defined on the schizo component's
 * command line in order to be accepted, but PRRTE will automatically translate
 * them to their accepted synonym */
#define PRTE_CLI_MACHINEFILE    "machinefile"       // synonym for "hostfile"
#define PRTE_CLI_WD             "wd"                // synonym for "wdir


/* define the command line directives PRRTE recognizes */

// Placement directives
#define PRTE_CLI_SLOT       "slot"
#define PRTE_CLI_HWT        "hwthread"
#define PRTE_CLI_CORE       "core"
#define PRTE_CLI_L1CACHE    "l1cache"
#define PRTE_CLI_L2CACHE    "l2cache"
#define PRTE_CLI_L3CACHE    "l3cache"
#define PRTE_CLI_NUMA       "numa"
#define PRTE_CLI_PACKAGE    "package"
#define PRTE_CLI_NODE       "node"
#define PRTE_CLI_SEQ        "seq"
#define PRTE_CLI_DIST       "dist"
#define PRTE_CLI_PPR        "ppr"
#define PRTE_CLI_RANKFILE   "rankfile"
#define PRTE_CLI_NONE       "none"
#define PRTE_CLI_HWTCPUS    "hwtcpus"
#define PRTE_CLI_PELIST     "pe-list="

// Ranking directives
// PRTE_CLI_SLOT, PRTE_CLI_NODE, PRTE_CLI_SPAN reused here
#define PRTE_CLI_FILL       "fill"
#define PRTE_CLI_OBJ        "object"


// Output directives
#define PRTE_CLI_TAG        "tag"
#define PRTE_CLI_TAG_DET    "tag-detailed"
#define PRTE_CLI_TAG_FULL   "tag-fullname"
#define PRTE_CLI_RANK       "rank"
#define PRTE_CLI_TIMESTAMP  "timestamp"
#define PRTE_CLI_XML        "xml"
#define PRTE_CLI_MERGE_ERROUT   "merge-stderr-to-stdout"
#define PRTE_CLI_DIR        "directory"
#define PRTE_CLI_FILE       "filename"

// Display directives
#define PRTE_CLI_ALLOC      "allocation"
#define PRTE_CLI_MAP        "map"
#define PRTE_CLI_BIND       "bind"
#define PRTE_CLI_MAPDEV     "map-devel"
#define PRTE_CLI_TOPO       "topo="

// Runtime directives
#define PRTE_CLI_ABORT_NZ           "abort-nonzero-status"          // optional arg
#define PRTE_CLI_NOLAUNCH           "donotlaunch"                   // no arg
#define PRTE_CLI_SHOW_PROGRESS      "show-progress"                 // optional arg
#define PRTE_CLI_RECOVER            "recover"                       // optional arg
#define PRTE_CLI_CONTINUOUS         "continuous"                    // optional arg
#define PRTE_CLI_MAX_RESTARTS       "max-restarts"                  // reqd arg
#define PRTE_CLI_EXEC_AGENT         "exec-agent"                    // reqd arg
#define PRTE_CLI_STOP_ON_EXEC       "stop-on-exec"                  // optional arg
#define PRTE_CLI_STOP_IN_INIT       "stop-in-init"                  // optional arg
#define PRTE_CLI_STOP_IN_APP        "stop-in-app"                   // optional arg
#define PRTE_CLI_TIMEOUT            "timeout"                       // reqd arg
#define PRTE_CLI_SPAWN_TIMEOUT      "spawn-timeout"                 // reqd arg
#define PRTE_CLI_REPORT_STATE       "report-state-on-timeout"       // optional arg
#define PRTE_CLI_STACK_TRACES       "get-stack-traces"              // optional arg
#define PRTE_CLI_REPORT_CHILD_SEP   "report-child-jobs-separately"  // optional arg
#define PRTE_CLI_AGG_HELP           "aggregate-help"                // optional arg


/* define the command line qualifiers PRRTE recognizes */

// Placement qualifiers
#define PRTE_CLI_PE         "pe="
#define PRTE_CLI_SPAN       "span"
#define PRTE_CLI_OVERSUB    "oversubscribe"
#define PRTE_CLI_NOOVER     "nooversubscribe"
#define PRTE_CLI_NOLOCAL    "nolocal"
// PRTE_CLI_HWTCPUS reused here
#define PRTE_CLI_CORECPUS   "corecpus"
#define PRTE_CLI_DEVICE     "device="
#define PRTE_CLI_INHERIT    "inherit"
#define PRTE_CLI_NOINHERIT  "noinherit"
#define PRTE_CLI_QDIR       "dir="
#define PRTE_CLI_QFILE      "file="
#define PRTE_CLI_OVERLOAD   "overload-allowed"
#define PRTE_CLI_NOOVERLOAD "no-overload"
#define PRTE_CLI_IF_SUPP    "if-supported"
#define PRTE_CLI_ORDERED    "ordered"
#define PRTE_CLI_REPORT     "report"
#define PRTE_CLI_DISPALLOC  "displayalloc"
// PRTE_CLI_DISPLAY reused here
#define PRTE_CLI_DISPDEV    "displaydevel"

// Output qualifiers
#define PRTE_CLI_NOCOPY     "nocopy"
#define PRTE_CLI_RAW        "raw"
#define PRTE_CLI_PATTERN    "pattern"

/* USAGE:
 *  param "a" is the input command line string
 *  param "b" is the defined CLI option
 */
static inline bool prte_check_cli_option(char *a, char *b)
{
    size_t len1, len2, len, n;
    char **asplit, **bsplit;
    bool match;

    /* if there exists a '-' in b, then we are
     * dealing with a multi-word option. Parse
     * those by checking each word segment
     * individually for a match so the user
     * doesn't have to spell it all out
     * unless necessary */
    if (NULL != strchr(b, '-')) {
        asplit = pmix_argv_split(a, '-');
        bsplit = pmix_argv_split(b, '-');
        match = false;
        for (n=0; NULL != asplit[n] && NULL != bsplit[n]; n++) {
            len1 = strlen(asplit[n]);
            len2 = strlen(bsplit[n]);
            len = len1 < len2 ? len1 : len2;
            if (0 == strncasecmp(asplit[n], bsplit[n], len)) {
                match = true;
            } else {
                pmix_argv_free(asplit);
                pmix_argv_free(bsplit);
                return false;
            }
        }
        pmix_argv_free(asplit);
        pmix_argv_free(bsplit);
        return match;
    }

    /* if this is not a multi-word option, we just
     * check the strings */
    len1 = strlen(a);
    len2 = strlen(b);
    len = (len1 < len2) ? len1 : len2;
    if (0 == strncasecmp(a, b, len)) {
        return true;
    }

    return false;
}

#define PRTE_CHECK_CLI_OPTION(a, b) \
    prte_check_cli_option(a, b)

/* check if an option is "true" */
static inline bool prte_check_true(char *a)
{
    int n;
    size_t len1, len;
    char *negs[] = {
        "false",
        "0",
        "no",
        NULL
    };

    if (NULL == a) {
        return true;  // default
    }
    len1 = strlen(a);

    for (n=0; NULL != negs[n]; n++) {
        len = (len1 < strlen(negs[n])) ? len1 : strlen(negs[n]);
        if (0 == strncasecmp(a, negs[n], len)) {
            return false;
        }
    }
    return true;
}
#define PRTE_CHECK_TRUE(a) \
    prte_check_true(a)

/* check if an option is a boolean */
static inline bool prte_check_bool(char *a)
{
    int n;
    size_t len1, len;
    char *negs[] = {
        "false",
        "0",
        "no",
        NULL
    };
    char *trues[] = {
        "true",
        "yes",
        "1",
        NULL
    };

    if (NULL == a) {
        return true;  // default
    }
    len1 = strlen(a);

    /* see if it is a negative (false) */
    for (n=0; NULL != negs[n]; n++) {
        len = (len1 < strlen(negs[n])) ? len1 : strlen(negs[n]);
        if (0 == strncasecmp(a, negs[n], len)) {
            return true;
        }
    }

    /* see if it is a positive (true) */
    for (n=0; NULL != trues[n]; n++) {
        len = (len1 < strlen(trues[n])) ? len1 : strlen(trues[n]);
        if (0 == strncasecmp(a, trues[n], len)) {
            return true;
        }
    }

    /* anything unrecognized must not be a boolean */
    return false;
}

#define PRTE_CHECK_BOOL(a) \
    prte_check_bool(a)

static unsigned int prte_convert_string_to_time(const char *t)
{
    char **tmp = pmix_argv_split(t, ':');
    int sz = pmix_argv_count(tmp);
    unsigned int tm;

    /* work upwards from the bottom, where the
     * bottom represents seconds, then minutes,
     * then hours, and then days */
    tm = strtoul(tmp[sz-1], NULL, 10);
    if (0 <= (sz-2) && NULL != tmp[sz-2]) {
        tm += 60 * strtoul(tmp[sz-2], NULL, 10);
    }
    if (0 <= (sz-3) && NULL != tmp[sz-3]) {
        tm += 60 * 60 * strtoul(tmp[sz-3], NULL, 10);
    }
    if (0 <= (sz-4) && NULL != tmp[sz-4]) {
        tm += 24 * 60 * 60 * strtoul(tmp[sz-4], NULL, 10);
    }
    pmix_argv_free(tmp);
    return tm;
}

#define PRTE_CONVERT_TIME(s)    \
    prte_convert_string_to_time(s)


END_C_DECLS

#endif /* PRTE_CMD_LINE_H */
