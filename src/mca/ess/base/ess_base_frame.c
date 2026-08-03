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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>

/* the highest signal number this platform can deliver; a few systems still
 * lack the definition, so fall back to the historical minimum as the
 * stacktrace code does */
#ifndef _NSIG
#    define _NSIG 32
#endif

#include "src/mca/base/pmix_base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/mca.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/proc_info.h"

#include "src/mca/ess/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * module's public pmix_mca_base_module_t struct.
 */

#include "src/mca/ess/base/static-components.h"

prte_ess_base_module_t prte_ess = {
    .init = NULL,
    .finalize = NULL,
};
int prte_ess_base_num_procs = -1;
char *prte_ess_base_nspace = NULL;
char *prte_ess_base_vpid = NULL;
pmix_list_t prte_ess_base_signals = PMIX_LIST_STATIC_INIT;

static char *forwarded_signals = "all";

static int prte_ess_base_register(pmix_mca_base_register_flag_t flags)
{
    int ret;
    PRTE_HIDE_UNUSED_PARAMS(flags);

    prte_ess_base_nspace = NULL;
    ret = pmix_mca_base_var_register("prte", "ess", "base", "nspace", "Process nspace",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_ess_base_nspace);
    pmix_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "nspace",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_ess_base_vpid = NULL;
    ret = pmix_mca_base_var_register("prte", "ess", "base", "vpid", "Process vpid",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &prte_ess_base_vpid);
    pmix_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "vpid",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_ess_base_num_procs = -1;
    ret = pmix_mca_base_var_register("prte", "ess", "base", "num_procs",
                                     "Used to discover the number of procs in the job",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_ess_base_num_procs);
    pmix_mca_base_var_register_synonym(ret, "prte", "prte", "ess", "num_procs",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    forwarded_signals = "all";
    ret = pmix_mca_base_var_register("prte", "ess", "base", "forward_signals",
                                     "Comma-delimited list of signals (names or integers) to be forwarded to "
                                     "application processes [\"none\" => forward nothing, \"all\" => forward all]. "
                                     "Signals provided by default depends upon system definitions. The SIGTERM, SIGHUP, "
                                     "SIGINT signals are always forwarded regardless of this param's settings. The "
                                     "SIGKILL and SIGPIPE signals cannot be forwarded.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &forwarded_signals);
    pmix_mca_base_var_register_synonym(ret, "prte", "ess", "hnp", "forward_signals",
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    return PRTE_SUCCESS;
}

static int prte_ess_base_close(void)
{
    PMIX_LIST_DESTRUCT(&prte_ess_base_signals);

    return pmix_mca_base_framework_components_close(&prte_ess_base_framework, NULL);
}

static int prte_ess_base_open(pmix_mca_base_open_flag_t flags)
{
    PMIX_CONSTRUCT(&prte_ess_base_signals, pmix_list_t);

    return pmix_mca_base_framework_components_open(&prte_ess_base_framework, flags);
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(prte, ess, "PRTE Environmenal System Setup", prte_ess_base_register,
                                prte_ess_base_open, prte_ess_base_close,
                                prte_ess_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

/* daemon identity */

/* Read @c envar as a plain non-negative integer.  Returns PRTE_ERR_NOT_FOUND
 * if it is unset, so the caller can log it as the internal error it is, and
 * PRTE_ERR_SILENT if it holds anything else - the specific diagnostic has
 * already been shown by then, and the caller's generic startup-failure
 * message must not be printed on top of it. */
static int get_env_index(const char *envar, unsigned long *val)
{
    char *str, *endp;

    str = getenv(envar);
    if (NULL == str) {
        return PRTE_ERR_NOT_FOUND;
    }
    errno = 0;
    *val = strtoul(str, &endp, 10);
    if (0 != errno || endp == str || '\0' != *endp || '-' == str[0]) {
        pmix_show_help("help-ess-base.txt", "ess-base:bad-identity", true, envar, str);
        return PRTE_ERR_SILENT;
    }
    return PRTE_SUCCESS;
}

int prte_ess_base_set_identity(const char *offset_envar, int offset_adjust)
{
    unsigned long vpid, offset = 0;
    char *endp;
    long rank;
    int rc;

    if (NULL == prte_ess_base_nspace) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    if (NULL == prte_ess_base_vpid) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }

    /* the base vpid must be a plain non-negative number */
    errno = 0;
    vpid = strtoul(prte_ess_base_vpid, &endp, 10);
    if (0 != errno || endp == prte_ess_base_vpid || '\0' != *endp
        || '-' == prte_ess_base_vpid[0]) {
        pmix_show_help("help-ess-base.txt", "ess-base:bad-identity", true,
                       "ess_base_vpid", prte_ess_base_vpid);
        return PRTE_ERR_SILENT;
    }

    /* add this node's index within the RM's allocation, when the RM is the
     * one distinguishing its daemons */
    if (NULL != offset_envar) {
        rc = get_env_index(offset_envar, &offset);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_NOT_FOUND == rc) {
                PRTE_ERROR_LOG(rc);
            }
            return rc;
        }
    }

    /* Bound each term before adding them so the sum cannot overflow, then
     * check it as a signed value: that is what catches the LSF adjustment
     * (-1) applied to a zero index, which would otherwise wrap around to a
     * rank up near UINT32_MAX. */
    if (PMIX_RANK_VALID <= vpid || PMIX_RANK_VALID <= offset) {
        rank = -1;
    } else {
        rank = (long) vpid + (long) offset + (long) offset_adjust;
    }
    /* compare the sum as a long: PMIX_RANK_IS_VALID would take a pmix_rank_t
     * and the narrowing conversion is what we are trying to catch */
    if (0 > rank || (long) PMIX_RANK_VALID <= rank) {
        pmix_show_help("help-ess-base.txt", "ess-base:rank-out-of-range", true,
                       prte_ess_base_vpid,
                       (NULL == offset_envar) ? "none" : offset_envar,
                       offset, offset_adjust);
        return PRTE_ERR_SILENT;
    }

    PMIX_LOAD_NSPACE(PRTE_PROC_MY_NAME->nspace, prte_ess_base_nspace);
    PRTE_PROC_MY_NAME->rank = (pmix_rank_t) rank;

    /* the number of daemons in the DVM came along with the identity */
    prte_process_info.num_daemons = prte_ess_base_num_procs;

    PMIX_OUTPUT_VERBOSE((1, prte_ess_base_framework.framework_output,
                         "ess: set name to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    return PRTE_SUCCESS;
}

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
    {0, NULL, false},
};

#define ESS_ADDSIGNAL(x, s)                                     \
    do {                                                        \
        prte_ess_base_signal_t *_sig;                           \
        _sig = PMIX_NEW(prte_ess_base_signal_t);                \
        _sig->signal = (x);                                     \
        _sig->signame = strdup((s));                            \
        pmix_list_append(&prte_ess_base_signals, &_sig->super); \
    } while (0)

static bool signals_added = false;

pmix_status_t prte_ess_base_setup_signals(char *input)
{
    int i, sval, nsigs;
    char *mysignals, **signals, *tmp, *sname=NULL;
    prte_ess_base_signal_t *sig;
    bool ignore, found;

    if (NULL == input) {
        mysignals = forwarded_signals;
    } else {
        mysignals = input;
    }

    /* if they told us "none", then nothing to do */
    if (0 == strcasecmp(mysignals, "none") || signals_added) {
        return PMIX_SUCCESS;
    }

    // handle the "all" special case
    if (0 == strcasecmp(mysignals, "all")) {
        nsigs = sizeof(known_signals) / sizeof(struct known_signal);
        for (i = 0; i < nsigs; i++) {
            if (known_signals[i].can_forward) {
                pmix_output_verbose(2,  prte_ess_base_framework.framework_output,
                                    "Forwarding signal: %s", known_signals[i].signame);
                ESS_ADDSIGNAL(known_signals[i].signal, known_signals[i].signame);
            }
        }
        signals_added = true;
        return PMIX_SUCCESS;
    }

    /* see what they asked for - ignore any duplicates */
    signals = PMIx_Argv_split(mysignals, ',');
    for (i = 0; NULL != signals[i]; i++) {
        sval = 0;
        if (0 != strncasecmp(signals[i], "SIG", 3)) {
            /* treat it like a number */
            errno = 0;
            sval = strtoul(signals[i], &tmp, 10);
            if (0 != errno || '\0' != *tmp) {
                pmix_show_help("help-ess-base.txt", "ess-base:unknown-signal", true,
                               signals[i], mysignals);
                PMIx_Argv_free(signals);
                return PMIX_ERR_SILENT;
            }
            /* A number outside the range this platform can deliver is no
             * more forwardable than an unknown name.  strtoul would happily
             * take "-1" (wrapping it) or "999", and the handler install that
             * follows fails silently on both - so the user's signal is never
             * forwarded and nothing ever says why. */
            if (0 >= sval || _NSIG <= sval) {
                pmix_show_help("help-ess-base.txt", "ess-base:unknown-signal", true,
                               signals[i], mysignals);
                PMIx_Argv_free(signals);
                return PMIX_ERR_SILENT;
            }
            // see if it's a known signal number
            sname = NULL;
            for (int j = 0; NULL != known_signals[j].signame; ++j) {
                if (sval == known_signals[j].signal) {
                    /* enforce the same forwardability gate as the name
                     * branch - a non-forwardable signal must be rejected
                     * whether it was given by name or by number */
                    if (!known_signals[j].can_forward) {
                        pmix_show_help("help-ess-base.txt", "ess-base:cannot-forward", true,
                                       known_signals[j].signame, mysignals);
                        PMIx_Argv_free(signals);
                        return PMIX_ERR_SILENT;
                    }
                    sname = known_signals[j].signame;
                    break;
                }
            }
            if (NULL == sname) {
                sname = signals[i];
            }
        } else {
            /* they gave us a signal name */
            found = false;
            for (int j = 0; NULL != known_signals[j].signame; ++j) {
                /* compare by name only: sval is still 0 on this branch, so
                 * the numeric test that used to sit here could never match
                 * anything (no signal is 0) */
                if (0 == strcasecmp(signals[i], known_signals[j].signame)) {
                    if (!known_signals[j].can_forward) {
                        pmix_show_help("help-ess-base.txt", "ess-base:cannot-forward", true,
                                       known_signals[j].signame, mysignals);
                        PMIx_Argv_free(signals);
                        return PMIX_ERR_SILENT;
                    }
                    found = true;
                    sval = known_signals[j].signal;
                    sname = known_signals[j].signame;
                    break;
                }
            }
            if (!found) {
                pmix_show_help("help-ess-base.txt", "ess-base:unknown-signal", true,
                               signals[i], mysignals);
                PMIx_Argv_free(signals);
                return PMIX_ERR_SILENT;
            }
        }

        /* see if it is one we already covered */
        ignore = false;
        PMIX_LIST_FOREACH(sig, &prte_ess_base_signals, prte_ess_base_signal_t) {
            if (0 == strcasecmp(sname, sig->signame) || sval == sig->signal) {
                /* got it - we will ignore */
                ignore = true;
                break;
            }
        }

        if (ignore) {
            continue;
        }

        pmix_output_verbose(2,  prte_ess_base_framework.framework_output,
                            "Forwarding signal: %s", sname);
        ESS_ADDSIGNAL(sval, sname);
    }

    PMIx_Argv_free(signals);
    /* Latch only now that the list is fully built.  Latching on entry would
     * burn the one allowed pass on a request that was rejected partway
     * through, leaving whatever it had appended installed and no way to
     * replace it - and it would make every rejection above unreachable a
     * second time, so none of them could be exercised. */
    signals_added = true;
    return PMIX_SUCCESS;
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
PMIX_CLASS_INSTANCE(prte_ess_base_signal_t, pmix_list_item_t, scon, sdes);
