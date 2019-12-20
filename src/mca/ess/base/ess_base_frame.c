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
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include <signal.h>

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"
#include "src/util/show_help.h"

#include "src/mca/ess/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * module's public prrte_mca_base_module_t struct.
 */

#include "src/mca/ess/base/static-components.h"

prrte_ess_base_module_t prrte_ess = {
    NULL,  /* init */
    NULL,  /* finalize */
    NULL,  /* abort */
    NULL   /* ft_event */
};
int prrte_ess_base_std_buffering = -1;
int prrte_ess_base_num_procs = -1;
char *prrte_ess_base_jobid = NULL;
char *prrte_ess_base_vpid = NULL;
prrte_list_t prrte_ess_base_signals = {{0}};

static prrte_mca_base_var_enum_value_t stream_buffering_values[] = {
  {-1, "default"},
  {0, "unbuffered"},
  {1, "line_buffered"},
  {2, "fully_buffered"},
  {0, NULL}
};

static int setup_signals(void);
static char *forwarded_signals = NULL;

static int prrte_ess_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_mca_base_var_enum_t *new_enum;
    int ret;

    prrte_ess_base_std_buffering = -1;
    (void) prrte_mca_base_var_enum_create("ess_base_stream_buffering", stream_buffering_values, &new_enum);
    (void) prrte_mca_base_var_register("prrte", "ess", "base", "stream_buffering",
                                       "Adjust buffering for stdout/stderr "
                                       "[-1 system default] [0 unbuffered] [1 line buffered] [2 fully buffered] "
                                       "(Default: -1)",
                                       PRRTE_MCA_BASE_VAR_TYPE_INT, new_enum, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ess_base_std_buffering);
    PRRTE_RELEASE(new_enum);

    prrte_ess_base_jobid = NULL;
    ret = prrte_mca_base_var_register("prrte", "ess", "base", "jobid", "Process jobid",
                                      PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                      PRRTE_INFO_LVL_9,
                                      PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ess_base_jobid);
    prrte_mca_base_var_register_synonym(ret, "prrte", "prrte", "ess", "jobid", 0);

    prrte_ess_base_vpid = NULL;
    ret = prrte_mca_base_var_register("prrte", "ess", "base", "vpid", "Process vpid",
                                      PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                      PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                      PRRTE_INFO_LVL_9,
                                      PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ess_base_vpid);
    prrte_mca_base_var_register_synonym(ret, "prrte", "prrte", "ess", "vpid", 0);

    prrte_ess_base_num_procs = -1;
    ret = prrte_mca_base_var_register("prrte", "ess", "base", "num_procs",
                                      "Used to discover the number of procs in the job",
                                      PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                      PRRTE_INFO_LVL_9,
                                      PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ess_base_num_procs);
    prrte_mca_base_var_register_synonym(ret, "prrte", "prrte", "ess", "num_procs", 0);

    forwarded_signals = NULL;
    ret = prrte_mca_base_var_register ("prrte", "ess", "base", "forward_signals",
                                       "Comma-delimited list of additional signals (names or integers) to forward to "
                                       "application processes [\"none\" => forward nothing]. Signals provided by "
                                       "default include SIGTSTP, SIGUSR1, SIGUSR2, SIGABRT, SIGALRM, and SIGCONT",
                                       PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                       PRRTE_INFO_LVL_4, PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &forwarded_signals);
    prrte_mca_base_var_register_synonym(ret, "prrte", "ess", "hnp", "forward_signals", 0);


    return PRRTE_SUCCESS;
}

static int prrte_ess_base_close(void)
{
    PRRTE_LIST_DESTRUCT(&prrte_ess_base_signals);

    return prrte_mca_base_framework_components_close(&prrte_ess_base_framework, NULL);
}

static int prrte_ess_base_open(prrte_mca_base_open_flag_t flags)
{
    int rc;

    PRRTE_CONSTRUCT(&prrte_ess_base_signals, prrte_list_t);

    if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_DAEMON) {
        if (PRRTE_SUCCESS != (rc = setup_signals())) {
            return rc;
        }
    }
    return prrte_mca_base_framework_components_open(&prrte_ess_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, ess, "PRRTE Environmenal System Setup",
                                 prrte_ess_base_register, prrte_ess_base_open, prrte_ess_base_close,
                                 prrte_ess_base_static_components, 0);

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

#define ESS_ADDSIGNAL(x, s)                                                 \
    do {                                                                    \
        prrte_ess_base_signal_t *_sig;                                       \
        _sig = PRRTE_NEW(prrte_ess_base_signal_t);                             \
        _sig->signal = (x);                                                 \
        _sig->signame = strdup((s));                                        \
        prrte_list_append(&prrte_ess_base_signals, &_sig->super);             \
    } while(0)

static int setup_signals(void)
{
    int i, sval, nsigs;
    char **signals, *tmp;
    prrte_ess_base_signal_t *sig;
    bool ignore, found;

    /* if they told us "none", then nothing to do */
    if (NULL != forwarded_signals &&
        0 == strcmp(forwarded_signals, "none")) {
        return PRRTE_SUCCESS;
    }

    /* we know that some signals are (nearly) always defined, regardless
     * of environment, so add them here */
    nsigs = sizeof(known_signals) / sizeof(struct known_signal);
    for (i=0; i < nsigs; i++) {
        if (known_signals[i].can_forward) {
            ESS_ADDSIGNAL(known_signals[i].signal, known_signals[i].signame);
        }
    }

    /* see if they asked for anything beyond those - note that they may
     * have asked for some we already cover, and so we ignore any duplicates */
    if (NULL != forwarded_signals) {
        /* if they told us "none", then dump the list */
        signals = prrte_argv_split(forwarded_signals, ',');
        for (i=0; NULL != signals[i]; i++) {
            sval = 0;
            if (0 != strncmp(signals[i], "SIG", 3)) {
                /* treat it like a number */
                errno = 0;
                sval = strtoul(signals[i], &tmp, 10);
                if (0 != errno || '\0' != *tmp) {
                    prrte_show_help("help-ess-base.txt", "ess-base:unknown-signal",
                                   true, signals[i], forwarded_signals);
                    prrte_argv_free(signals);
                    return PRRTE_ERR_SILENT;
                }
            }

            /* see if it is one we already covered */
            ignore = false;
            PRRTE_LIST_FOREACH(sig, &prrte_ess_base_signals, prrte_ess_base_signal_t) {
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
            for (int j = 0 ; known_signals[j].signame ; ++j) {
                if (0 == strcasecmp (signals[i], known_signals[j].signame) || sval == known_signals[j].signal) {
                    if (!known_signals[j].can_forward) {
                        prrte_show_help("help-ess-base.txt", "ess-base:cannot-forward",
                                       true, known_signals[j].signame, forwarded_signals);
                        prrte_argv_free(signals);
                        return PRRTE_ERR_SILENT;
                    }
                    found = true;
                    ESS_ADDSIGNAL(known_signals[j].signal, known_signals[j].signame);
                    break;
                }
            }

            if (!found) {
                if (0 == strncmp(signals[i], "SIG", 3)) {
                    prrte_show_help("help-ess-base.txt", "ess-base:unknown-signal",
                                   true, signals[i], forwarded_signals);
                    prrte_argv_free(signals);
                    return PRRTE_ERR_SILENT;
                }

                ESS_ADDSIGNAL(sval, signals[i]);
            }
        }
        prrte_argv_free (signals);
    }
    return PRRTE_SUCCESS;
}

/* instantiate the class */
static void scon(prrte_ess_base_signal_t *t)
{
    t->signame = NULL;
}
static void sdes(prrte_ess_base_signal_t *t)
{
    if (NULL != t->signame) {
        free(t->signame);
    }
}
PRRTE_CLASS_INSTANCE(prrte_ess_base_signal_t,
                   prrte_list_item_t,
                   scon, sdes);
