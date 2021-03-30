/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <signal.h>

#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/show_help.h"

#include "src/mca/ess/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * module's public prte_mca_base_module_t struct.
 */

#include "src/mca/ess/base/static-components.h"

prte_ess_base_module_t prte_ess = {
    NULL, /* init */
    NULL, /* finalize */
    NULL, /* abort */
};
int prte_ess_base_std_buffering = -1;
int prte_ess_base_num_procs = -1;
char *prte_ess_base_nspace = NULL;
char *prte_ess_base_vpid = NULL;
prte_list_t prte_ess_base_signals = {{0}};

static prte_mca_base_var_enum_value_t stream_buffering_values[] = {{-1, "default"},
                                                                   {0, "unbuffered"},
                                                                   {1, "line_buffered"},
                                                                   {2, "fully_buffered"},
                                                                   {0, NULL}};

static char *forwarded_signals = NULL;

static int prte_ess_base_register(prte_mca_base_register_flag_t flags)
{
    prte_mca_base_var_enum_t *new_enum;
    int ret;

    prte_ess_base_std_buffering = -1;
    (void) prte_mca_base_var_enum_create("ess_base_stream_buffering", stream_buffering_values,
                                         &new_enum);
    (void) prte_mca_base_var_register(
        "prte", "ess", "base", "stream_buffering",
        "Adjust buffering for stdout/stderr "
        "[-1 system default] [0 unbuffered] [1 line buffered] [2 fully buffered] "
        "(Default: -1)",
        PRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ess_base_std_buffering);
    PRTE_RELEASE(new_enum);

    prte_ess_base_nspace = NULL;
    ret = prte_mca_base_var_register("prte", "ess", "base", "nspace", "Process nspace",
                                     PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                     PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                     PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ess_base_nspace);
    prte_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "nspace",
                                       PRTE_MCA_BASE_VAR_SYN_FLAG_NONE);

    prte_ess_base_vpid = NULL;
    ret = prte_mca_base_var_register("prte", "ess", "base", "vpid", "Process vpid",
                                     PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                     PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                     PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ess_base_vpid);
    prte_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "vpid",
                                       PRTE_MCA_BASE_VAR_SYN_FLAG_NONE);

    prte_ess_base_num_procs = -1;
    ret = prte_mca_base_var_register("prte", "ess", "base", "num_procs",
                                     "Used to discover the number of procs in the job",
                                     PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
                                     PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ess_base_num_procs);
    prte_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "num_procs",
                                       PRTE_MCA_BASE_VAR_SYN_FLAG_NONE);

    forwarded_signals = NULL;
    ret = prte_mca_base_var_register(
        "prte", "ess", "base", "forward_signals",
        "Comma-delimited list of additional signals (names or integers) to forward to "
        "application processes [\"none\" => forward nothing]. Signals provided by "
        "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_4,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &forwarded_signals);
    prte_mca_base_var_register_synonym(ret, "prte", "ess", "hnp", "forward_signals",
                                       PRTE_MCA_BASE_VAR_SYN_FLAG_NONE);

    return PRTE_SUCCESS;
}

static int prte_ess_base_close(void)
{
    PRTE_LIST_DESTRUCT(&prte_ess_base_signals);

    return prte_mca_base_framework_components_close(&prte_ess_base_framework, NULL);
}

static int prte_ess_base_open(prte_mca_base_open_flag_t flags)
{
    int rc;

    PRTE_CONSTRUCT(&prte_ess_base_signals, prte_list_t);

    if (PRTE_SUCCESS != (rc = prte_ess_base_setup_signals(forwarded_signals))) {
        return rc;
    }

    return prte_mca_base_framework_components_open(&prte_ess_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, ess, "PRTE Environmenal System Setup", prte_ess_base_register,
                                prte_ess_base_open, prte_ess_base_close,
                                prte_ess_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

/* signal forwarding */

/* setup signal forwarding list */
struct known_signal {
    /** signal number */
    int signal;
    /** signal name */
    char *signame;
    /** can this signal be forwarded */
    bool can_forward;
};

static struct known_signal known_signals[] = {
    {SIGTERM, "SIGTERM", false},
    {SIGHUP, "SIGHUP", false},
    {SIGINT, "SIGINT", false},
    {SIGKILL, "SIGKILL", false},
    {SIGPIPE, "SIGPIPE", false},
#ifdef SIGQUIT
    {SIGQUIT, "SIGQUIT", false},
#endif
#ifdef SIGTRAP
    {SIGTRAP, "SIGTRAP", true},
#endif
#ifdef SIGTSTP
    {SIGTSTP, "SIGTSTP", true},
#endif
#ifdef SIGABRT
    {SIGABRT, "SIGABRT", true},
#endif
#ifdef SIGCONT
    {SIGCONT, "SIGCONT", true},
#endif
#ifdef SIGSYS
    {SIGSYS, "SIGSYS", true},
#endif
#ifdef SIGXCPU
    {SIGXCPU, "SIGXCPU", true},
#endif
#ifdef SIGXFSZ
    {SIGXFSZ, "SIGXFSZ", true},
#endif
#ifdef SIGALRM
    {SIGALRM, "SIGALRM", true},
#endif
#ifdef SIGVTALRM
    {SIGVTALRM, "SIGVTALRM", true},
#endif
#ifdef SIGPROF
    {SIGPROF, "SIGPROF", true},
#endif
#ifdef SIGINFO
    {SIGINFO, "SIGINFO", true},
#endif
#ifdef SIGPWR
    {SIGPWR, "SIGPWR", true},
#endif
#ifdef SIGURG
    {SIGURG, "SIGURG", true},
#endif
#ifdef SIGUSR1
    {SIGUSR1, "SIGUSR1", true},
#endif
#ifdef SIGUSR2
    {SIGUSR2, "SIGUSR2", true},
#endif
    {0, NULL},
};

#define ESS_ADDSIGNAL(x, s)                                     \
    do {                                                        \
        prte_ess_base_signal_t *_sig;                           \
        _sig = PRTE_NEW(prte_ess_base_signal_t);                \
        _sig->signal = (x);                                     \
        _sig->signame = strdup((s));                            \
        prte_list_append(&prte_ess_base_signals, &_sig->super); \
    } while (0)

static bool signals_added = false;

int prte_ess_base_setup_signals(char *mysignals)
{
    int i, sval, nsigs;
    char **signals, *tmp;
    prte_ess_base_signal_t *sig;
    bool ignore, found;

    /* if they told us "none", then nothing to do */
    if (NULL != mysignals && 0 == strcmp(mysignals, "none")) {
        return PRTE_SUCCESS;
    }

    if (!signals_added) {
        /* we know that some signals are (nearly) always defined, regardless
         * of environment, so add them here */
        nsigs = sizeof(known_signals) / sizeof(struct known_signal);
        for (i = 0; i < nsigs; i++) {
            if (known_signals[i].can_forward) {
                ESS_ADDSIGNAL(known_signals[i].signal, known_signals[i].signame);
            }
        }
        signals_added = true; // only do this once
    }

    /* see if they asked for anything beyond those - note that they may
     * have asked for some we already cover, and so we ignore any duplicates */
    if (NULL != mysignals) {
        /* if they told us "none", then dump the list */
        signals = prte_argv_split(mysignals, ',');
        for (i = 0; NULL != signals[i]; i++) {
            sval = 0;
            if (0 != strncmp(signals[i], "SIG", 3)) {
                /* treat it like a number */
                errno = 0;
                sval = strtoul(signals[i], &tmp, 10);
                if (0 != errno || '\0' != *tmp) {
                    prte_show_help("help-ess-base.txt", "ess-base:unknown-signal", true, signals[i],
                                   forwarded_signals);
                    prte_argv_free(signals);
                    return PRTE_ERR_SILENT;
                }
            }

            /* see if it is one we already covered */
            ignore = false;
            PRTE_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t)
            {
                if (0 == strcasecmp(signals[i], sig->signame) || sval == sig->signal) {
                    /* got it - we will ignore */
                    ignore = true;
                    break;
                }
            }

            if (ignore) {
                continue;
            }

            /* see if they gave us a signal name */
            found = false;
            for (int j = 0; known_signals[j].signame; ++j) {
                if (0 == strcasecmp(signals[i], known_signals[j].signame)
                    || sval == known_signals[j].signal) {
                    if (!known_signals[j].can_forward) {
                        prte_show_help("help-ess-base.txt", "ess-base:cannot-forward", true,
                                       known_signals[j].signame, forwarded_signals);
                        prte_argv_free(signals);
                        return PRTE_ERR_SILENT;
                    }
                    found = true;
                    ESS_ADDSIGNAL(known_signals[j].signal, known_signals[j].signame);
                    break;
                }
            }

            if (!found) {
                if (0 == strncmp(signals[i], "SIG", 3)) {
                    prte_show_help("help-ess-base.txt", "ess-base:unknown-signal", true, signals[i],
                                   forwarded_signals);
                    prte_argv_free(signals);
                    return PRTE_ERR_SILENT;
                }

                ESS_ADDSIGNAL(sval, signals[i]);
            }
        }
        prte_argv_free(signals);
    }
    return PRTE_SUCCESS;
}

/* instantiate the class */
static void scon(prte_ess_base_signal_t *t)
{
    t->signame = NULL;
}
static void sdes(prte_ess_base_signal_t *t)
{
    if (NULL != t->signame) {
        free(t->signame);
    }
}
PRTE_CLASS_INSTANCE(prte_ess_base_signal_t, prte_list_item_t, scon, sdes);
