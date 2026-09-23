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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "pmix_common.h"

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
#define PRTE_CLI_DEFAULT_HOSTFILE       "default-hostfile"          // required
#define PRTE_CLI_SINGLETON              "singleton"                 // required
#define PRTE_CLI_KEEPALIVE              "keepalive"                 // required
#define PRTE_CLI_LAUNCH_AGENT           "launch-agent"              // required
#define PRTE_CLI_MAX_VM_SIZE            "max-vm-size"               // required
/* NOTE: "--debug" is accepted only as a deprecated no-op (the option
 * tables carry it as a literal in their deprecated sections, and
 * convert_deprecated_cli warns and drops it).  There is deliberately no
 * PRTE_CLI_DEBUG here: a canonical name would invite a new consumer for
 * an option no tool acts on. */
#define PRTE_CLI_DEBUG_DAEMONS          "debug-daemons"             // none
#define PRTE_CLI_DEBUG_DAEMONS_FILE     "debug-daemons-file"        // none
#define PRTE_CLI_LEAVE_SESSION_ATTACHED "leave-session-attached"    // none
#define PRTE_CLI_TMPDIR                 "tmpdir"                    // required
#define PRTE_CLI_PREFIX                 "prefix"                    // required
#define PRTE_CLI_PMIX_PREFIX			"pmix-prefix"				// required
#define PRTE_CLI_APP_PREFIX				"app-pmix-prefix"			// required
#define PRTE_CLI_NOPREFIX               "noprefix"                  // none
#define PRTE_CLI_NO_APP_PREFIX   	    "no-app-prefix"             // none
#define PRTE_CLI_FWD_SIGNALS            "forward-signals"           // required
#define PRTE_CLI_RUN_AS_ROOT            "allow-run-as-root"         // none
#define PRTE_CLI_STREAM_BUF             "stream-buffering"          // required
#define PRTE_CLI_BOOTSTRAP              "bootstrap"                 // none
#define PRTE_CLI_HOMO_NODES             "uniform-nodes"             // none

// Application options
#define PRTE_CLI_NP                     "np"                        // required
#define PRTE_CLI_NPERNODE               "N"                         // required
#define PRTE_CLI_APPFILE                "app"                       // required
#define PRTE_CLI_FWD_ENVAR              "x"                         // required
#define PRTE_CLI_FWD_ENVIRON            "fwd-environment"           // optional
#define PRTE_CLI_HOSTFILE               "hostfile"                  // required
#define PRTE_CLI_ADDHOSTFILE            "add-hostfile"              // required
#define PRTE_CLI_HOST                   "host"                      // required
#define PRTE_CLI_ADDHOST                "add-host"                  // required
#define PRTE_CLI_ACTIVATE               "activate"                  // required
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
#define PRTE_CLI_MEM_ALLOC_KIND			"memory-alloc-kinds"        // required
#define PRTE_CLI_GPU_SUPPORT			"gpu-support"				// required
#define PRTE_CLI_TARGET_ALLOC           "alloc-id"                  // required
#define PRTE_CLI_ALLOC_REFID            "alloc-refid"               // required
#define PRTE_CLI_SESSION_ID             "session-id"                // required

// Placement options
#define PRTE_CLI_MAPBY                  "mapby"                     // required
#define PRTE_CLI_RANKBY                 "rankby"                    // required
#define PRTE_CLI_BINDTO                 "bindto"                    // required

// Runtime options
#define PRTE_CLI_RTOS                   "rtos"           			// required

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
#define PRTE_CLI_DVM                    "dvm"                       // optional

// Daemon-specific CLI options
#define PRTE_CLI_PUBSUB_SERVER          "pubsub-server"             // required
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

// Placement directives - used by mapping and binding
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
#define PRTE_CLI_DEVICE     "device="
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
#define PRTE_CLI_ALLOC      	"allocation"
#define PRTE_CLI_MAP        	"map"
#define PRTE_CLI_BIND       	"bind"
#define PRTE_CLI_MAPDEV     	"map-devel"
#define PRTE_CLI_TOPO       	"topo="
#define PRTE_CLI_CPUS       	"cpus="
#define PRTE_CLI_PHYSICAL_CPUS 	"physical"

// Runtime directives
#define PRTE_CLI_ERROR_NZ           "error-nonzero-status"          // optional arg
#define PRTE_CLI_NOLAUNCH           "donotlaunch"                   // no arg
#define PRTE_CLI_NOSPAWN            "donotspawn"                    // no arg
#define PRTE_CLI_SHOW_PROGRESS      "show-progress"                 // optional arg
#define PRTE_CLI_RECOVERABLE        "recoverable"                   // optional arg
#define PRTE_CLI_AUTORESTART        "autorestart"                   // optional arg
#define PRTE_CLI_CONTINUOUS         "continuous"                    // optional arg
#define PRTE_CLI_MAX_RESTARTS       "max-restarts"                  // reqd arg
#define PRTE_CLI_EXEC_AGENT         "exec-agent"                    // reqd arg
#define PRTE_CLI_DEFAULT_EXEC_AGENT "default-exec-agent"            // no arg
#define PRTE_CLI_STOP_ON_EXEC       "stop-on-exec"                  // optional arg
#define PRTE_CLI_STOP_IN_INIT       "stop-in-init"                  // optional arg
#define PRTE_CLI_STOP_IN_APP        "stop-in-app"                   // optional arg
#define PRTE_CLI_TIMEOUT            "timeout"                       // reqd arg
#define PRTE_CLI_SPAWN_TIMEOUT      "spawn-timeout"                 // reqd arg
#define PRTE_CLI_REPORT_STATE       "report-state-on-timeout"       // optional arg
#define PRTE_CLI_STACK_TRACES       "get-stack-traces"              // optional arg
#define PRTE_CLI_REPORT_CHILD_SEP   "report-child-jobs-separately"  // optional arg
// the full name is what the runtime-options help text and the MCA param
// description both document; the option matcher accepts any unambiguous
// prefix, so the shorter "aggregate-help" still works. Naming the SHORT
// form here had the opposite effect - a directive longer than the name
// matches no prefix of it, so the documented spelling was rejected.
#define PRTE_CLI_AGG_HELP           "aggregate-help-messages"       // optional arg
#define PRTE_CLI_NOTIFY_ERRORS      "notifyerrors"                  // optional flag
#define PRTE_CLI_OUTPUT_PROCTABLE   "output-proctable"              // optional arg


/* define the command line qualifiers PRRTE recognizes */

// Placement qualifiers
#define PRTE_CLI_PE         "pe="
#define PRTE_CLI_SPAN       "span"
#define PRTE_CLI_OVERSUB    "oversubscribe"
#define PRTE_CLI_NOOVER     "nooversubscribe"
#define PRTE_CLI_NOLOCAL    "nolocal"
// PRTE_CLI_HWTCPUS reused here
#define PRTE_CLI_CORECPUS   "corecpus"
#define PRTE_CLI_INHERIT    "inherit"
#define PRTE_CLI_NOINHERIT  "noinherit"
#define PRTE_CLI_QDIR       "dir="
#define PRTE_CLI_QFILE      "file="
#define PRTE_CLI_OVERLOAD   "overload-allowed"
#define PRTE_CLI_NOOVERLOAD "no-overload"
#define PRTE_CLI_IF_SUPP    "if-supported"
#define PRTE_CLI_ORDERED    "ordered"
#define PRTE_CLI_INTERLEAVE "interleave"
#define PRTE_CLI_SHARED     "shared"
#define PRTE_CLI_NDEV       "ndev"
#define PRTE_CLI_REPORT     "report"
#define PRTE_CLI_DISPALLOC  "displayalloc"
// PRTE_CLI_DISPLAY reused here
#define PRTE_CLI_DISPDEV    "displaydevel"
#define PRTE_CLI_LIMIT      "limit="

// Output qualifiers
#define PRTE_CLI_COPY       "copy"
#define PRTE_CLI_NOCOPY     "nocopy"
#define PRTE_CLI_RAW        "raw"
#define PRTE_CLI_PATTERN    "pattern"

/*
 * The value of a qualifier declared above with a trailing '=' - PE=2,
 * FILE=path, LIMIT=4 - is read with pmix_cli_qualifier_value(), which comes
 * from PMIx alongside the option matchers: the two belong together,
 * because the matchers accept any abbreviation of a qualifier's name and
 * the caller therefore cannot know how long the name it matched was.
 * Never index past the qualifier's full spelling to reach its value.
 *
 * There is deliberately no local fallback for a PMIx that lacks it.  A
 * second implementation of a rule this easy to get wrong is a second thing
 * to keep right; configure refuses such a PMIx instead.
 */

/*
 * The vocabularies of the options whose values are themselves a small
 * language - "--map-by package:span:pe=2", "--output tag,file=out" - one
 * table per set of words a user chooses from.
 *
 * Each vocabulary is defined here ONCE, and both of its readers use it:
 * the sanity checker in schizo, which refuses a bad command line before
 * anything acts on it, and the parser that acts on a good one.  They used
 * to keep a list apiece and the lists drifted - the checker let
 * "ppr:2:slot" through to a parser that has never known what to do with
 * it, and the parser accepted spellings of a ppr object the checker then
 * refused.
 *
 * A word is matched with pmix_cli_match() against the WHOLE table, so an
 * abbreviation that fits two entries is refused as ambiguous rather than
 * settled by whichever one a chain of comparisons happened to test first,
 * and a value given to a word that takes none is refused rather than
 * dropped.  Entries sharing a tag are spellings of one thing.  The tag is
 * what the parser switches on - never the position in the table.
 */

/* --map-by: the policy word */
typedef enum {
    PRTE_MAPPER_SLOT,
    PRTE_MAPPER_HWT,
    PRTE_MAPPER_CORE,
    PRTE_MAPPER_L1CACHE,
    PRTE_MAPPER_L2CACHE,
    PRTE_MAPPER_L3CACHE,
    PRTE_MAPPER_NUMA,
    PRTE_MAPPER_PACKAGE,
    PRTE_MAPPER_NODE,
    PRTE_MAPPER_SEQ,
    PRTE_MAPPER_PPR,
    PRTE_MAPPER_RANKFILE,
    PRTE_MAPPER_PELIST,
    PRTE_MAPPER_DEVICE
} prte_cli_mapper_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_mappers[];

/* --map-by: the qualifiers */
typedef enum {
    PRTE_MAPQUAL_PE,
    PRTE_MAPQUAL_SPAN,
    PRTE_MAPQUAL_OVERSUB,
    PRTE_MAPQUAL_NOOVER,
    PRTE_MAPQUAL_NOLOCAL,
    PRTE_MAPQUAL_HWTCPUS,
    PRTE_MAPQUAL_CORECPUS,
    PRTE_MAPQUAL_INHERIT,
    PRTE_MAPQUAL_NOINHERIT,
    PRTE_MAPQUAL_FILE,
    PRTE_MAPQUAL_ORDERED,
    PRTE_MAPQUAL_INTERLEAVE,
    PRTE_MAPQUAL_SHARED,
    PRTE_MAPQUAL_NDEV
} prte_cli_mapqual_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_mapquals[];

/* --map-by ppr:N:<object> - the object */
typedef enum {
    PRTE_PPROBJ_NODE,
    PRTE_PPROBJ_HWT,
    PRTE_PPROBJ_CORE,
    PRTE_PPROBJ_PACKAGE,
    PRTE_PPROBJ_NUMA,
    PRTE_PPROBJ_L1CACHE,
    PRTE_PPROBJ_L2CACHE,
    PRTE_PPROBJ_L3CACHE,
    PRTE_PPROBJ_DEVICE
} prte_cli_pprobj_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_ppr_objects[];

/* --rank-by: the policy word.  It takes no qualifiers. */
typedef enum {
    PRTE_RANKER_SLOT,
    PRTE_RANKER_NODE,
    PRTE_RANKER_FILL,
    PRTE_RANKER_SPAN
} prte_cli_ranker_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_rankers[];

/* --bind-to: the policy word */
typedef enum {
    PRTE_BINDER_NONE,
    PRTE_BINDER_HWT,
    PRTE_BINDER_CORE,
    PRTE_BINDER_L1CACHE,
    PRTE_BINDER_L2CACHE,
    PRTE_BINDER_L3CACHE,
    PRTE_BINDER_NUMA,
    PRTE_BINDER_PACKAGE
} prte_cli_binder_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_binders[];

/* --bind-to: the qualifiers */
typedef enum {
    PRTE_BINDQUAL_OVERLOAD,
    PRTE_BINDQUAL_NOOVERLOAD,
    PRTE_BINDQUAL_IF_SUPP,
    PRTE_BINDQUAL_LIMIT,
    PRTE_BINDQUAL_REPORT
} prte_cli_bindqual_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_bindquals[];

/* --output: the directives, then their qualifiers */
typedef enum {
    PRTE_OUTPUT_TAG,
    PRTE_OUTPUT_TAG_DET,
    PRTE_OUTPUT_TAG_FULL,
    PRTE_OUTPUT_RANK,
    PRTE_OUTPUT_TIMESTAMP,
    PRTE_OUTPUT_XML,
    PRTE_OUTPUT_MERGE_ERROUT,
    PRTE_OUTPUT_DIR,
    PRTE_OUTPUT_FILE
} prte_cli_output_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_output_directives[];

typedef enum {
    PRTE_OUTQUAL_COPY,
    PRTE_OUTQUAL_NOCOPY,
    PRTE_OUTQUAL_RAW,
    PRTE_OUTQUAL_PATTERN
} prte_cli_outqual_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_output_quals[];

/* --display: the directives, then their qualifiers */
typedef enum {
    PRTE_DISPLAY_ALLOC,
    PRTE_DISPLAY_MAP,
    PRTE_DISPLAY_BIND,
    PRTE_DISPLAY_MAPDEV,
    PRTE_DISPLAY_TOPO,
    PRTE_DISPLAY_CPUS
} prte_cli_display_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_display_directives[];

typedef enum {
    PRTE_DISPQUAL_PARSEABLE,
    PRTE_DISPQUAL_PHYSICAL
} prte_cli_dispqual_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_display_quals[];

/* --rtos: the directives.  They take no qualifiers. */
typedef enum {
    PRTE_RTOS_ERROR_NZ,
    PRTE_RTOS_NOLAUNCH,
    PRTE_RTOS_NOSPAWN,
    PRTE_RTOS_SHOW_PROGRESS,
    PRTE_RTOS_RECOVERABLE,
    PRTE_RTOS_AUTORESTART,
    PRTE_RTOS_CONTINUOUS,
    PRTE_RTOS_MAX_RESTARTS,
    PRTE_RTOS_EXEC_AGENT,
    PRTE_RTOS_DEFAULT_EXEC_AGENT,
    PRTE_RTOS_STOP_ON_EXEC,
    PRTE_RTOS_STOP_IN_INIT,
    PRTE_RTOS_STOP_IN_APP,
    PRTE_RTOS_TIMEOUT,
    PRTE_RTOS_SPAWN_TIMEOUT,
    PRTE_RTOS_REPORT_STATE,
    PRTE_RTOS_STACK_TRACES,
    PRTE_RTOS_REPORT_CHILD_SEP,
    PRTE_RTOS_AGG_HELP,
    PRTE_RTOS_NOTIFY_ERRORS,
    PRTE_RTOS_OUTPUT_PROCTABLE,
    PRTE_RTOS_FWD_ENVIRON
} prte_cli_rtos_t;
PRTE_EXPORT extern const pmix_cli_choice_t prte_cli_rtos_directives[];

/**
 * Match one word of a directive against a vocabulary, and explain any
 * failure other than "matches nothing".
 *
 * An ambiguous abbreviation, and a value given to a word that takes none
 * or withheld from one that needs it, are reported here - one message
 * for each, whichever option they turn up in.  An input that matches
 * nothing is NOT reported, because each option already has its own
 * message for that listing what it does accept, and the caller is the one
 * that knows which.
 *
 * @param nspace   the job the command line belongs to, for show_help
 * @param option   the option being parsed ("map-by"), for the message
 * @param input    the word as the user wrote it, with any "=value"
 * @param choices  the vocabulary
 * @param tag      set to the matched entry's tag on success
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_NOT_FOUND  matches nothing - not yet reported
 * @retval PRTE_ERR_SILENT     reported
 */
PRTE_EXPORT int prte_cli_match(const pmix_nspace_t nspace, const char *option,
                               const char *input, const pmix_cli_choice_t *choices,
                               int *tag);

/**
 * The name a vocabulary gives a tag - the first spelling it lists - for
 * whatever has to name what the user asked for in full, which they may
 * have abbreviated.  NULL if no entry carries the tag.
 */
PRTE_EXPORT const char *prte_cli_name(const pmix_cli_choice_t *choices, int tag);

/*
 * Interpreters for option values that more than one tool accepts.  These
 * live here, and not in a tool's main(), because a tool's main() cannot be
 * unit tested - see test/unit/tools.
 */

/**
 * Interpret the optional value carried by a boolean directive or qualifier.
 *
 * Every boolean directive of "--output", "--display" and "--rtos" may be
 * written bare ("tag") or with a value ("tag=0", "tag=false", "tag=no").
 * The bare form is the assertion; the value form says the same thing out
 * loud, and is the only way to say the opposite.
 *
 * A value that is neither true nor false is REFUSED rather than read as
 * false.  The truth test underneath reports anything it does not recognize
 * as false, so "tag=maybe" would otherwise mean "no tags" - and "tag=0"
 * used to mean "tags", since the value was not looked at at all.  The
 * caller reports the error, since only the caller knows which directive of
 * which option was being written.
 *
 * @param value  the text after the '=' - NULL or empty for the bare form
 * @param flag   the truth the directive carries; bare == true
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_BAD_PARAM  the value is not a truth value
 */
PRTE_EXPORT int prte_cli_bool_value(const char *value, bool *flag);

/**
 * Interpret the value of "--pid": either a decimal PID, or "file:<path>"
 * naming a file whose first token is one.
 *
 * @param value     the option's value
 * @param pid       filled in on success
 * @param filename  if non-NULL, set to the path within @c value when the
 *                  "file:" form was used (borrowed, not a copy), else NULL.
 *                  Callers use it to name the file in an error message.
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_BAD_PARAM          neither form, or out of range
 * @retval PRTE_ERR_FILE_OPEN_FAILURE  the named file could not be opened
 * @retval PRTE_ERR_FILE_READ_FAILURE  the file held no readable PID
 */
PRTE_EXPORT int prte_parse_pid_option(const char *value, pid_t *pid,
                                      const char **filename);

/**
 * Append the contents of an appfile to an argument vector, one app
 * context per line, ':'-delimited as if they had been typed.  Each line
 * is split on spaces; blank lines and comment lines (first non-blank
 * character '#') are skipped.  This is the one "--app" reader, shared by
 * prun and prterun.
 *
 * @param filename  the appfile
 * @param argv      argv to append to - may already hold the tool's own
 *                  arguments, and is created if it points at NULL
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_FILE_OPEN_FAILURE
 */
PRTE_EXPORT int prte_load_appfile(const char *filename, char ***argv);

/**
 * Check that a command line carrying "--app <file>" names no application
 * of its own.
 *
 * The appfile is appended to the command line with no ':' in front of it,
 * so an executable (or a ':'-separated app segment) already on the command
 * line would swallow the file's first line as its own arguments.
 *
 * @param tail  the parser's command tail - everything from the first
 *              non-option token on (NULL when there is none)
 *
 * @retval PRTE_SUCCESS        the command line names no application
 * @retval PRTE_ERR_BAD_PARAM  it does
 */
PRTE_EXPORT int prte_check_appfile_tail(char **tail);

/**
 * Interpret an octal umask string, as handed to a daemon in
 * PRTE_DAEMON_UMASK_VALUE.
 *
 * @return true (and fills @c mask) only for a complete, in-range octal
 *         value - an empty or trailing-garbage string is refused rather
 *         than silently read as 0, which would leave every file the
 *         daemon creates world-writable.
 */
PRTE_EXPORT bool prte_parse_umask(const char *value, mode_t *mask);

/**
 * Interpret a command-line option value that has to be a non-negative
 * decimal number.
 *
 * strtoul() reports a perfectly successful zero for a string with no
 * digits in it, and zero is meaningful almost everywhere PRRTE asks for a
 * number - "no timeout", "let the mapping policy compute the count",
 * "rank 0".  So a misspelled value does not fail; it quietly means
 * something else.  This refuses anything that is not a complete run of
 * digits, and anything that does not fit the field the caller is going
 * to store it in.
 *
 * @param value   the option's value
 * @param limit   the largest value the caller's field can hold
 * @param result  filled in on success, zero otherwise
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_BAD_PARAM  not a number, or larger than @c limit
 */
PRTE_EXPORT int prte_parse_uint_option(const char *value, unsigned long limit,
                                       unsigned long *result);

/* One inclusive run of ranks named by "--xterm" */
typedef struct {
    uint32_t lo;
    uint32_t hi;
} prte_rank_range_t;

/**
 * Interpret the value of "--xterm".
 *
 * The value is either "all" (case-insensitive; "-1" is still accepted as
 * the old spelling of it) or a comma-delimited list of ranks and inclusive
 * rank ranges - e.g. "1,3-6,9". A trailing "!" asks that each window be
 * kept open after its process exits (xterm -hold).
 *
 * @param value    the option's value
 * @param ranges   on success, a malloc'd array of the ranges named - NULL
 *                 when @c all is set. Caller frees.
 * @param nranges  number of entries in @c ranges
 * @param all      set if every rank of the job is named
 * @param hold     set if the "!" suffix was given
 * @param badrank  set to the offending value on PRTE_ERR_VALUE_OUT_OF_BOUNDS
 *
 * @retval PRTE_SUCCESS
 * @retval PRTE_ERR_BAD_PARAM            not in the syntax above
 * @retval PRTE_ERR_VALUE_OUT_OF_BOUNDS  a negative rank was named
 */
PRTE_EXPORT int prte_parse_xterm_option(const char *value, prte_rank_range_t **ranges,
                                        size_t *nranges, bool *all, bool *hold,
                                        long *badrank);

/**
 * Does a parsed "--xterm" list name a given rank?
 */
PRTE_EXPORT bool prte_xterm_names_rank(const prte_rank_range_t *ranges, size_t nranges,
                                       bool all, uint32_t rank);

END_C_DECLS

#endif /* PRTE_CMD_LINE_H */
