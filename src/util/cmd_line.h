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
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
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

#include "src/class/prte_list.h"
#include "src/class/prte_object.h"
#include "src/util/argv.h"

BEGIN_C_DECLS

typedef struct {
    prte_list_item_t super;
    char *key;
    char **values;
} prte_cli_item_t;
PRTE_CLASS_DECLARATION(prte_cli_item_t);

typedef struct {
    prte_object_t super;
    prte_list_t instances;  // comprised of prte_cli_item_t's
    char **tail;  // remainder of argv
} prte_cli_result_t;
PRTE_CLASS_DECLARATION(prte_cli_result_t);

#define PRTE_CLI_RESULT_STATIC_INIT                 \
{                                                   \
    .super = PRTE_OBJ_STATIC_INIT(prte_object_t),   \
    .instances = PRTE_LIST_STATIC_INIT,             \
    .tail = NULL                                    \
}

/* define PRTE-named flags for argument required */
#define PRTE_ARG_REQD       required_argument
#define PRTE_ARG_NONE       no_argument
#define PRTE_ARG_OPTIONAL   optional_argument

/* define PRTE-named flags for whether parsing
 * CLI shall include deprecation warnings */
#define PRTE_CLI_SILENT     true
#define PRTE_CLI_WARN       false

/* define a long option that has no short option equivalent
 *
 * n = name of the option (see below for definitions)
 * a = whether or not it requires an argument
 */
#define PRTE_OPTION_DEFINE(n, a)    \
{                                   \
    .name = (n),                    \
    .has_arg = (a),                 \
    .flag = NULL,                   \
    .val = 0                        \
}
/* define a long option that has a short option equivalent
 *
 * n = name of the option (see below for definitions)
 * a = whether or not it requires an argument
 * c = single character equivalent option
 */
#define PRTE_OPTION_SHORT_DEFINE(n, a, c)   \
{                                           \
    .name = (n),                            \
    .has_arg = (a),                         \
    .flag = NULL,                           \
    .val = (c)                              \
}

#define PRTE_OPTION_END  {0, 0, 0, 0}

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
#define PRTE_CLI_DEBUG_DAEMONS          "debug-daemons"             // none
#define PRTE_CLI_DEBUG_DAEMONS_FILE     "debug-daemons-file"        // none
#define PRTE_CLI_LEAVE_SESSION_ATTACHED "leave-session-attached"    // none
#define PRTE_CLI_TMPDIR                 "tmpdir"                    // required
#define PRTE_CLI_PREFIX                 "prefix"                    // required
#define PRTE_CLI_NOPREFIX               "noprefix"                  // none
#define PRTE_CLI_FWD_SIGNALS            "forward-signals"           // required
#define PRTE_CLI_RUN_AS_ROOT            "allow-run-as-root"         // none

// Application options
#define PRTE_CLI_NP                     "np"                        // required
#define PRTE_CLI_NPERNODE               "N"                         // required
#define PRTE_CLI_APPFILE                "app"                       // required
#define PRTE_CLI_TIMEOUT                "timeout"                   // required
#define PRTE_CLI_SPAWN_TIMEOUT          "spawn-timeout"             // required
#define PRTE_CLI_REPORT_STATE           "report-state-on-timeout"   // none
#define PRTE_CLI_STACK_TRACES           "get-stack-traces"          // none
#define PRTE_CLI_FWD_ENVAR              "x"                         // required
#define PRTE_CLI_SHOW_PROGRESS          "show-progress"             // none
#define PRTE_CLI_HOSTFILE               "hostfile"                  // required
#define PRTE_CLI_HOST                   "host"                      // required
#define PRTE_CLI_STREAM_BUF             "stream-buffering"          // required
#define PRTE_CLI_REPORT_CHILD_SEP       "report-child-jobs-separately"  // none
#define PRTE_CLI_PATH                   "path"                      // required
#define PRTE_CLI_PSET                   "pset"                      // required
#define PRTE_CLI_PRELOAD_FILES          "preload-files"             // required
#define PRTE_CLI_PRELOAD_BIN            "preload-binary"            // none
#define PRTE_CLI_STDIN                  "stdin"                     // required
#define PRTE_CLI_OUTPUT                 "output"                    // required
#define PRTE_CLI_WDIR                   "wdir"                      // required
#define PRTE_CLI_SET_CWD_SESSION        "set-cwd-to-session-dir"    // none
#define PRTE_CLI_ENABLE_RECOVERY        "enable-recovery"           // none
#define PRTE_CLI_MAX_RESTARTS           "max-restarts"              // required
#define PRTE_CLI_DISABLE_RECOVERY       "disable-recovery"          // none
#define PRTE_CLI_CONTINUOUS             "continuous"                // none
#define PRTE_CLI_EXEC_AGENT             "exec-agent"                // required

// Placement options
#define PRTE_CLI_MAPBY                  "map-by"                    // required
#define PRTE_CLI_RANKBY                 "rank-by"                   // required
#define PRTE_CLI_BINDTO                 "bind-to"                   // required

// Debug options
#define PRTE_CLI_DO_NOT_LAUNCH          "do-not-launch"             // none
#define PRTE_CLI_DISPLAY                "display"                   // required
#define PRTE_CLI_XTERM                  "xterm"                     // none
#define PRTE_CLI_STOP_ON_EXEC           "stop-on-exec"              // none
#define PRTE_CLI_STOP_IN_INIT           "stop-in-init"              // required
#define PRTE_CLI_STOP_IN_APP            "stop-in-app"               // required

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

// Output directives
#define PRTE_CLI_TAG        "tag"
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
#define PRTE_CLI_TOPO       "topo"

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
#define PRTE_CLI_PELIST     "pe-list="
#define PRTE_CLI_QDIR       "dir="
#define PRTE_CLI_QFILE      "file="
#define PRTE_CLI_NOLAUNCH   "donotlaunch"
#define PRTE_CLI_FILL       "fill"
#define PRTE_CLI_OVERLOAD   "overload-allowed"
#define PRTE_CLI_NOOVERLOAD "no-overload"
#define PRTE_CLI_IF_SUPP    "if-supported"
#define PRTE_CLI_ORDERED    "ordered"
#define PRTE_CLI_REPORT     "report"
#define PRTE_CLI_DISPALLOC  "displayalloc"
// PRTE_CLI_DISPLAY reused here
#define PRTE_CLI_DISPDEV    "displaydevel"
#define PRTE_CLI_DISPTOPO   "displaytopo"

// Output qualifiers
#define PRTE_CLI_NOCOPY     "nocopy"
#define PRTE_CLI_RAW        "raw"
#define PRTE_CLI_PATTERN    "pattern"

typedef void (*prte_cmd_line_store_fn_t)(const char *name, const char *option,
                                         prte_cli_result_t *results);

PRTE_EXPORT int prte_cmd_line_parse(char **argv, char *shorts,
                                    struct option myoptions[],
                                    prte_cmd_line_store_fn_t storefn,
                                    prte_cli_result_t *results,
                                    char *helpfile);

static inline prte_cli_item_t* prte_cmd_line_get_param(prte_cli_result_t *results,
                                                       const char *key)
{
    prte_cli_item_t *opt;

    PRTE_LIST_FOREACH(opt, &results->instances, prte_cli_item_t) {
        if (0 == strcmp(opt->key, key)) {
            return opt;
        }
    }
    return NULL;
}

static inline bool prte_cmd_line_is_taken(prte_cli_result_t *results,
                                          const char *key)
{
    if (NULL == prte_cmd_line_get_param(results, key)) {
        return false;
    }
    return true;
}

static inline int prte_cmd_line_get_ninsts(prte_cli_result_t *results,
                                           const char *key)
{
    prte_cli_item_t *opt;

    opt = prte_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return 0;
    }
    return prte_argv_count(opt->values);
}

static inline char* prte_cmd_line_get_nth_instance(prte_cli_result_t *results,
                                                   const char *key, int idx)
{
    prte_cli_item_t *opt;
    int n, ninst;

    opt = prte_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return NULL;
    }
    ninst = prte_argv_count(opt->values);
    if (ninst < idx) {
        return NULL;
    }
    return opt->values[idx];
}

#define PRTE_CLI_DEBUG_LIST(r)  \
do {                                                                    \
    prte_cli_item_t *_c;                                                \
    prte_output(0, "\n[%s:%s:%d]", __FILE__, __func__, __LINE__);       \
    PRTE_LIST_FOREACH(_c, &(r)->instances, prte_cli_item_t) {           \
        prte_output(0, "KEY: %s", _c->key);                             \
        if (NULL != _c->values) {                                       \
            for (int _n=0; NULL != _c->values[_n]; _n++) {              \
                prte_output(0, "    VAL[%d]: %s", _n, _c->values[_n]);  \
            }                                                           \
        }                                                               \
    }                                                                   \
    prte_output(0, "\n");                                               \
} while(0)

#define PRTE_CLI_REMOVE_DEPRECATED(r, o)    \
do {                                                        \
    prte_list_remove_item(&(r)->instances, &(o)->super);    \
    PRTE_RELEASE(o);                                        \
} while(0)
END_C_DECLS

#endif /* PRTE_CMD_LINE_H */
