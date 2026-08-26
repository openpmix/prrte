/*
 * Copyright (c) 2022-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * PRRTE's implementation of the PMIx_Session_control host callback.
 *
 * A "session" here is a prte_session_t - PRRTE's allocation object. The
 * scheduler is the authority over sessions: it instantiates them onto a set
 * of nodes, operates on them while they run, and reclaims them. Requests
 * that reach a daemon from anyone else are relayed to the scheduler, which
 * decides and calls back down to us. When no scheduler is present the DVM
 * master is the authority and answers for itself, subject to the ownership
 * check every reservation carries.
 *
 * What PRRTE can and cannot do with each directive is recorded at the
 * operation that implements it. The two it cannot do at all are
 * PMIX_SESSION_PROVISION_NODES and PMIX_SESSION_PROVISION_IMAGE: PRRTE does
 * not image machines.
 */

#include "prte_config.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/rml/rml.h"
#include "src/util/dash_host/dash_host.h"
#include "src/mca/plm/plm.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/rmaps/rmaps.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

/* the mutually-exclusive operations a session control request may ask for */
typedef enum {
    PRTE_SESSCTRL_NONE = 0,
    PRTE_SESSCTRL_INSTANTIATE,
    PRTE_SESSCTRL_TERMINATE,
    PRTE_SESSCTRL_PAUSE,
    PRTE_SESSCTRL_RESUME,
    PRTE_SESSCTRL_PREEMPT,
    PRTE_SESSCTRL_RESTORE,
    PRTE_SESSCTRL_SIGNAL,
    PRTE_SESSCTRL_EXTEND,
    PRTE_SESSCTRL_COMPLETE,
    PRTE_SESSCTRL_MODIFY        /* no operation, only modifiers (e.g. SEP) */
} prte_sessctrl_op_t;

/* The parsed form of one request. Every pointer in here is BORROWED from the
 * caller's directive array, which PMIx keeps alive until we answer - with the
 * single exception of the nodelist/nspaces argv arrays, which we build and
 * which sessctrl_destruct frees.
 *
 * Borrowed from THAT array, and set_response() frees it: which is why
 * set_response() may run only once per parsed request, and why nothing may
 * read one of these pointers after it has. */
typedef struct {
    prte_sessctrl_op_t op;
    int nops;                   /* number of operations named - >1 is an error */
    char *ctrl_id;              /* PMIX_SESSION_CTRL_ID, echoed in the answer */
    pmix_proc_t requestor;      /* PMIX_REQUESTOR if given, else the caller */
    bool have_sep;
    bool sep;
    /* instantiation */
    pmix_info_t *jobinfo;       /* PMIX_SESSION_JOB payload */
    size_t njobinfo;
    pmix_app_t *apps;           /* PMIX_SESSION_APP payload */
    size_t napps;
    pmix_info_t *personality;
    char **nodelists;           /* rendered PMIX_ALLOC_NODE_LIST strings */
    char *alloc_refid;          /* PMIX_ALLOC_ID */
    char *user_refid;           /* PMIX_ALLOC_REQ_ID */
    uid_t uid;
    bool have_uid;
    uint8_t inheritance;        /* PMIX_ALLOC_INHERITANCE disposition */
    bool have_inheritance;
    long timelimit;             /* seconds; -1 if not specified */
    /* operational */
    int sigvalue;
    char **nspaces;             /* PMIX_NSPACE targets for preempt/restore */
} prte_sessctrl_t;

static void sessctrl_construct(prte_sessctrl_t *ctl)
{
    memset(ctl, 0, sizeof(prte_sessctrl_t));
    PMIX_LOAD_PROCID(&ctl->requestor, NULL, PMIX_RANK_INVALID);
    ctl->timelimit = -1;
    ctl->uid = PRTE_INVALID_UID;
    ctl->inheritance = PRTE_INHERIT_DEFAULT_VALUE;
}

static void sessctrl_destruct(prte_sessctrl_t *ctl)
{
    if (NULL != ctl->nodelists) {
        PMIx_Argv_free(ctl->nodelists);
        ctl->nodelists = NULL;
    }
    if (NULL != ctl->nspaces) {
        PMIx_Argv_free(ctl->nspaces);
        ctl->nspaces = NULL;
    }
}

/* Read a string out of one directive.
 *
 * Every value in this request came from the caller's own info array: PMIx
 * hands what a process passed to PMIx_Session_control straight up to the host
 * without inspecting what any entry holds, and this upcall is reachable by
 * any application process or tool attached to any daemon.  So reading a fixed
 * member of the value union reads whatever eight bytes the caller chose to
 * put there - and every string taken here goes on to a strdup, a strlen or a
 * parser, which is the fault version of the rule rather than the quiet one.
 *
 * Refuse a mistyped or absent value rather than treating it as not given:
 * "not given" has a meaning for all of these and it is a different answer,
 * not an error. */
static pmix_status_t get_string_directive(pmix_info_t *info, char **out)
{
    /* Define the out-parameter before anything can fail, the way
     * prte_ras_base_parse_node_list() does.  A caller has no business
     * reading it without checking the status, but leaving it untouched on
     * the refusal path makes that a garbage pointer rather than a NULL -
     * and leaves the compiler unable to prove the caller's variable is ever
     * written, which is what GCC's -Wmaybe-uninitialized was saying. */
    *out = NULL;
    if (PMIX_STRING != info->value.type || NULL == info->value.data.string) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: directive %s carries no string",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), info->key);
        return PMIX_ERR_BAD_PARAM;
    }
    *out = info->value.data.string;
    return PMIX_SUCCESS;
}

/*****   DIRECTIVE PARSING   *****/

/* Convert a PMIX_ALLOC_TIME specification into seconds. The format is
 * "months:days:hours:minutes:seconds" scanned from the RIGHT, so a bare "2"
 * means two seconds and "1:30" means a minute and a half. A month is taken as
 * 30 days, which is the only fixed value available without a calendar date to
 * anchor it. Returns -1 if the string is not a well-formed time.
 *
 * Exported solely so test/unit/prted can cover it: it is a pure function of
 * its argument, and it is the one place a scheduler's time limit is
 * interpreted. */
long prte_pmix_server_parse_session_time(const char *str)
{
    /* multiplier for each field, right to left */
    static const long mult[] = {1, 60, 3600, 86400, 2592000};
    char **fields;
    int n, nf, ncolons = 0;
    long total = 0, val;
    char *end;
    const char *p;

    if (NULL == str || '\0' == str[0]) {
        return -1;
    }
    for (p = str; '\0' != *p; p++) {
        if (':' == *p) {
            ncolons++;
        }
    }
    fields = PMIx_Argv_split(str, ':');
    if (NULL == fields) {
        return -1;
    }
    nf = PMIx_Argv_count(fields);
    if (0 == nf || nf > (int) (sizeof(mult) / sizeof(mult[0]))) {
        PMIx_Argv_free(fields);
        return -1;
    }
    /* PMIx_Argv_split DISCARDS empty tokens, so ":30", "1:" and "1::2" all
     * come back as a shorter field list rather than as an error - and each
     * would then be read as a different, perfectly plausible duration. A
     * misread time limit terminates a user's session at the wrong moment, so
     * reject anything whose separator count does not match the fields it
     * produced. */
    if (ncolons != nf - 1) {
        PMIx_Argv_free(fields);
        return -1;
    }
    /* walk right to left so the rightmost field is always seconds */
    for (n = 0; n < nf; n++) {
        const char *f = fields[nf - 1 - n];
        errno = 0;
        val = strtol(f, &end, 10);
        /* strtol always hands back a non-NULL end pointer aimed at the first
         * character it did not consume, so the test is on *end */
        if (0 != errno || '\0' != *end || 0 > val) {
            PMIx_Argv_free(fields);
            return -1;
        }
        /* strtol reports only its OWN overflow; the multiply and the sum are
         * ours, and signed overflow is undefined rather than merely wrong.
         * "3555555555555:0:0:0:0" is inside strtol's range and outside this
         * one. */
        if (val > (LONG_MAX - total) / mult[n]) {
            PMIx_Argv_free(fields);
            return -1;
        }
        total += val * mult[n];
    }
    PMIx_Argv_free(fields);
    return total;
}

/* record that another operation was named - the caller rejects nops > 1 */
static void set_op(prte_sessctrl_t *ctl, prte_sessctrl_op_t op, bool flag)
{
    if (!flag) {
        return;
    }
    ctl->op = op;
    ctl->nops++;
}

/* Scan the resource description carried by PMIX_SESSION_RESOURCES. Only the
 * node-naming forms mean anything to PRRTE: it does not select machines, it is
 * told which ones it has. A request that asks PRRTE to choose (a node COUNT, a
 * cpu count, a memory size) is refused rather than silently granted whatever
 * the caller happened to also name. */
static pmix_status_t scan_resources(prte_sessctrl_t *ctl,
                                    pmix_info_t *info, size_t ninfo)
{
    size_t n;
    pmix_status_t rc;
    char *ndstring, *tstr = NULL;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NODE_LIST)) {
            rc = prte_ras_base_parse_node_list(&info[n], &ndstring);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            PMIx_Argv_append_nosize(&ctl->nodelists, ndstring);
            free(ndstring);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_ID)) {
            rc = get_string_directive(&info[n], &ctl->alloc_refid);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_REQ_ID)) {
            rc = get_string_directive(&info[n], &ctl->user_refid);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_TIME)) {
            rc = get_string_directive(&info[n], &tstr);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            ctl->timelimit = prte_pmix_server_parse_session_time(tstr);
            if (0 > ctl->timelimit) {
                return PMIX_ERR_BAD_PARAM;
            }

#if defined(PMIX_ALLOC_INHERITANCE)
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_INHERITANCE)) {
            if (!prte_pmix_value_get_inheritance(&info[n].value, &ctl->inheritance)) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->have_inheritance = true;
#endif

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NUM_NODES) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NUM_CPUS) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NUM_CPU_LIST) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_CPU_LIST) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_MEM_SIZE) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_FABRIC) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_QUEUE)) {
            /* these describe resources PRRTE would have to go and find - that
             * is the scheduler's job, and PRRTE is the thing being scheduled */
            pmix_output_verbose(2, prte_pmix_server_globals.output,
                                "%s session ctrl: unsupported resource directive %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), info[n].key);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        /* anything else in the resource description is ignored - it may well
         * be meaningful to a different RTE */
    }
    return PMIX_SUCCESS;
}

/* Walk the request once, recording everything. Nothing is acted on here: an
 * info array has no defined order, and the operations below depend on values
 * that may appear after them (the whole reason the previous single-pass
 * version mis-handled an INSTANTIATE that preceded its SESSION_JOB). */
static pmix_status_t parse_directives(prte_pmix_server_req_t *req,
                                      prte_sessctrl_t *ctl)
{
    size_t n;
    pmix_status_t rc;
    char *ndstring, *tstr = NULL;
    pmix_data_array_t *darray;

    /* the caller is the requestor unless a relaying daemon named the original */
    PMIX_XFER_PROCID(&ctl->requestor, &req->tproc);

    for (n = 0; n < req->ninfo; n++) {
        pmix_info_t *info = &req->info[n];

        if (PMIX_CHECK_KEY(info, PMIX_SESSION_CTRL_ID)) {
            rc = get_string_directive(info, &ctl->ctrl_id);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_REQUESTOR)) {
            if (PMIX_PROC != info->value.type || NULL == info->value.data.proc) {
                return PMIX_ERR_BAD_PARAM;
            }
            PMIX_XFER_PROCID(&ctl->requestor, info->value.data.proc);

            /***   INSTANTIATION   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_INSTANTIATE)) {
            set_op(ctl, PRTE_SESSCTRL_INSTANTIATE, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_RESOURCES)) {
            darray = info->value.data.darray;
            if (PMIX_DATA_ARRAY != info->value.type || NULL == darray ||
                PMIX_INFO != darray->type) {
                return PMIX_ERR_BAD_PARAM;
            }
            rc = scan_resources(ctl, (pmix_info_t *) darray->array, darray->size);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_JOB)) {
            darray = info->value.data.darray;
            if (PMIX_DATA_ARRAY != info->value.type || NULL == darray ||
                PMIX_INFO != darray->type) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->jobinfo = (pmix_info_t *) darray->array;
            ctl->njobinfo = darray->size;

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_APP)) {
            darray = info->value.data.darray;
            if (PMIX_DATA_ARRAY != info->value.type || NULL == darray ||
                PMIX_APP != darray->type) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->apps = (pmix_app_t *) darray->array;
            ctl->napps = darray->size;

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_PROVISION_NODES) ||
                   PMIX_CHECK_KEY(info, PMIX_SESSION_PROVISION_IMAGE)) {
            /* PRRTE does not provision machines - it runs on the ones it is
             * given, in the state it finds them */
            return PMIX_ERR_NOT_SUPPORTED;

        } else if (PMIX_CHECK_KEY(info, PMIX_PERSONALITY)) {
            /* checked here rather than where it is read: build_session_job()
             * splits it and hands it to the schizo proxy detector */
            rc = get_string_directive(info, &tstr);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            ctl->personality = info;

        } else if (PMIX_CHECK_KEY(info, PMIX_USERID)) {
            uint32_t uid;
            /* PMIx has no data type for uid_t, so read it as the unsigned
             * 32-bit quantity every platform PRRTE builds on makes it */
            rc = PMIx_Value_get_number(&info->value, &uid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->uid = (uid_t) uid;
            ctl->have_uid = true;

        } else if (PMIX_CHECK_KEY(info, PMIX_GRPID)) {
            /* Accepted and ignored. PRRTE never changes credentials - every
             * process it starts runs as the user running the DVM - so a group
             * id is not something it can act on. The user id IS recorded,
             * because reservation ownership is checked against it. */
            continue;

            /***   RESOURCES NAMED DIRECTLY ON THE REQUEST   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_NODE_LIST)) {
            rc = prte_ras_base_parse_node_list(info, &ndstring);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            PMIx_Argv_append_nosize(&ctl->nodelists, ndstring);
            free(ndstring);

        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_ID)) {
            rc = get_string_directive(info, &ctl->alloc_refid);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_REQ_ID)) {
            rc = get_string_directive(info, &ctl->user_refid);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_TIME)) {
            rc = get_string_directive(info, &tstr);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            ctl->timelimit = prte_pmix_server_parse_session_time(tstr);
            if (0 > ctl->timelimit) {
                return PMIX_ERR_BAD_PARAM;
            }

#if defined(PMIX_ALLOC_INHERITANCE)
        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_INHERITANCE)) {
            /* governs what becomes of the session's nodes when it is reclaimed:
             * handed back to the scheduler, or returned to the DVM's general
             * pool. See returns_to_scheduler().  Read through the shared
             * reader rather than off a fixed union member: the destructive
             * disposition must be one the caller asked for, not one a type
             * confusion produced. */
            if (!prte_pmix_value_get_inheritance(&info->value, &ctl->inheritance)) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->have_inheritance = true;
#endif

        } else if (PMIX_CHECK_KEY(info, PMIX_ALLOC_NUM_CPU_LIST) ||
                   PMIX_CHECK_KEY(info, PMIX_ALLOC_NUM_NODES) ||
                   PMIX_CHECK_KEY(info, PMIX_ALLOC_NUM_CPUS) ||
                   PMIX_CHECK_KEY(info, PMIX_ALLOC_MEM_SIZE)) {
            return PMIX_ERR_NOT_SUPPORTED;

            /***   OPERATIONS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_PAUSE)) {
            set_op(ctl, PRTE_SESSCTRL_PAUSE, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_RESUME)) {
            set_op(ctl, PRTE_SESSCTRL_RESUME, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_TERMINATE)) {
            set_op(ctl, PRTE_SESSCTRL_TERMINATE, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_PREEMPT)) {
            set_op(ctl, PRTE_SESSCTRL_PREEMPT, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_RESTORE)) {
            set_op(ctl, PRTE_SESSCTRL_RESTORE, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_COMPLETE)) {
            set_op(ctl, PRTE_SESSCTRL_COMPLETE, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_EXTEND)) {
            set_op(ctl, PRTE_SESSCTRL_EXTEND, PMIX_INFO_TRUE(info));

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_SIGNAL)) {
            rc = PMIx_Value_get_number(&info->value, &ctl->sigvalue, PMIX_INT);
            if (PMIX_SUCCESS != rc || 0 >= ctl->sigvalue) {
                return PMIX_ERR_BAD_PARAM;
            }
            set_op(ctl, PRTE_SESSCTRL_SIGNAL, true);

        } else if (PMIX_CHECK_KEY(info, PMIX_SESSION_SEP)) {
            ctl->have_sep = true;
            ctl->sep = PMIX_INFO_TRUE(info);

            /***   QUALIFIERS   ***/
        } else if (PMIX_CHECK_KEY(info, PMIX_NSPACE)) {
            /* Names a job within the session - preempt/restore/signal target
             * these.  It has to name one: nspace_named() matches with
             * PMIX_CHECK_NSPACE, which counts an EMPTY nspace as matching
             * anything, so an empty string here does not select no job, it
             * selects every job in the session - and then reports success
             * rather than the PMIX_ERR_NOT_FOUND a caller who named nothing
             * real is owed. */
            rc = get_string_directive(info, &tstr);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            if ('\0' == tstr[0]) {
                return PMIX_ERR_BAD_PARAM;
            }
            PMIx_Argv_append_nosize(&ctl->nspaces, tstr);

        } else if (PMIX_CHECK_KEY(info, PMIX_TIMEOUT)) {
            int64_t tl;
            /* PMIx has no data type for long, so read the widest signed
             * quantity it does have and refuse what a long cannot hold -
             * writing an int64_t through a long* would overrun the field
             * wherever long is 32 bits */
            rc = PMIx_Value_get_number(&info->value, &tl, PMIX_INT64);
            if (PMIX_SUCCESS != rc || 0 > tl || (int64_t) LONG_MAX < tl) {
                return PMIX_ERR_BAD_PARAM;
            }
            ctl->timelimit = (long) tl;
        }
        /* an unrecognized directive is not an error: the request may be
         * addressed at more than one RTE, and refusing on a key we simply
         * have no opinion about would break a caller we otherwise serve */
    }

    if (1 < ctl->nops) {
        /* the operations are alternatives, not a program - "pause and
         * terminate" has no meaning and no safe interpretation */
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: %d conflicting operations requested",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ctl->nops);
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 == ctl->nops) {
        /* a request carrying only modifiers still has work to do */
        ctl->op = (ctl->have_sep) ? PRTE_SESSCTRL_MODIFY : PRTE_SESSCTRL_NONE;
    }
    /* the header is explicit that job info without apps is an error - there is
     * nothing for it to describe */
    if (NULL != ctl->jobinfo && NULL == ctl->apps) {
        return PMIX_ERR_BAD_PARAM;
    }
    if ((NULL != ctl->apps || NULL != ctl->jobinfo) &&
        PRTE_SESSCTRL_INSTANTIATE != ctl->op) {
        /* apps are only launched as part of instantiating the session that is
         * to hold them */
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

/*****   OPERATIONS   *****/

/* True if the requestor may operate on this session. prte_session_is_owned_by
 * is the whole answer: it admits the scheduler (when one is connected),
 * the namespaces recorded as owners, and - for a tool - the user the
 * reservation was granted to, which is what lets a second command from the
 * same person reach an allocation their first command created. */
#define AUTHORIZED(s, r) prte_session_is_owned_by((s), (r)->nspace)

static bool nspace_named(char **nspaces, const pmix_nspace_t nspace)
{
    int i;

    if (NULL == nspaces) {
        return true;    /* no filter - every job matches */
    }
    for (i = 0; NULL != nspaces[i]; i++) {
        if (PMIX_CHECK_NSPACE(nspaces[i], nspace)) {
            return true;
        }
    }
    return false;
}

/* Deliver a signal to every job in the session, or to just the named ones.
 * When mark is true the jobs are additionally flagged suspended (or, when
 * clear is true, un-flagged), which is what makes a later resume/restore able
 * to tell a stopped job from a running one. */
static pmix_status_t session_signal(prte_session_t *session, char **nspaces,
                                    int32_t sig, bool mark, bool suspend)
{
    int k, rc;
    prte_job_t *jdata;
    bool found = false;
    pmix_status_t ret = PMIX_SUCCESS;

    for (k = 0; k < session->jobs->size; k++) {
        jdata = (prte_job_t *) pmix_pointer_array_get_item(session->jobs, k);
        if (NULL == jdata) {
            continue;
        }
        if (!nspace_named(nspaces, jdata->nspace)) {
            continue;
        }
        found = true;
        if (mark) {
            /* do not stop a job twice, and do not continue one that was never
             * stopped - either would leave the flag disagreeing with the
             * process state, and SIGCONT to a running job is a wasted xcast */
            if (suspend == (bool) PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_SUSPENDED)) {
                continue;
            }
        }
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: signal %d to job %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) sig,
                            PRTE_JOBID_PRINT(jdata->nspace));
        rc = prte_plm.signal_job(jdata->nspace, sig);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            ret = prte_pmix_convert_rc(rc);
            continue;
        }
        if (mark) {
            if (suspend) {
                PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_SUSPENDED);
            } else {
                PRTE_FLAG_UNSET(jdata, PRTE_JOB_FLAG_SUSPENDED);
            }
        }
    }

    if (NULL != nspaces && !found) {
        /* the caller named jobs and none of them are in this session */
        return PMIX_ERR_NOT_FOUND;
    }
    return ret;
}

/* Arm (or re-arm) the session's time limit. Passing 0 seconds disarms it. */
static void session_timeout_cb(int fd, short args, void *cbdata);

static void arm_session_timer(prte_session_t *session, long seconds)
{
    if (NULL != session->timer) {
        prte_event_evtimer_del(session->timer->ev);
        PMIX_RELEASE(session->timer);
        session->timer = NULL;
    }
    session->timeout.tv_sec = seconds;
    session->timeout.tv_usec = 0;
    if (0 >= seconds) {
        return;
    }
    session->timer = PMIX_NEW(prte_timer_t);
    session->timer->payload = session;
    prte_event_evtimer_set(prte_event_base, session->timer->ev,
                           session_timeout_cb, session);
    prte_event_evtimer_add(session->timer->ev, &session->timeout);
    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl: session %u time limit set to %ld sec",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        session->session_id, seconds);
}

/* True when the session's disposition says its nodes go back to the scheduler
 * rather than back to the DVM's general pool. Mirrors what
 * prte_ras_base_check_reservations_on_term applies on namespace termination,
 * and defaults - as that does - to unreserving into the DVM: handing the nodes
 * away is the destructive choice, and it must be the one the caller asked for
 * rather than the one it gets by saying nothing. */
static bool returns_to_scheduler(prte_session_t *session)
{
#if defined(PMIX_ALLOC_INHERIT_NONE)
    if (PMIX_ALLOC_INHERIT_NONE == session->inheritance) {
        return true;
    }
#endif
#if defined(PMIX_ALLOC_INHERIT_CHILD)
    if (PMIX_ALLOC_INHERIT_CHILD == session->inheritance) {
        return true;
    }
#endif
    return false;
}

/* Reclaim a session whose jobs have all retired: give up its hold on its
 * nodes and, if the scheduler instantiated it, tell the scheduler it is done.
 * Runs on the PRRTE progress thread. */
static void reclaim_session(prte_session_t *session)
{
    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl: reclaiming session %u",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), session->session_id);

    /* disarm the clock before the session goes - the event carries a bare
     * pointer to it */
    arm_session_timer(session, 0);

    /* Do the PRRTE-side teardown first, while we are on the thread that owns
     * these objects. Only then report completion, so the statement we make to
     * the scheduler - "all resources have been recovered" - is already true
     * when we make it. */
    prte_ras_base_teardown_reservation(session, returns_to_scheduler(session));

    prte_pmix_server_session_complete(session);
}

/* Terminate every job in the session. The session itself is reclaimed once
 * the last of them retires (prte_pmix_server_session_job_terminated), so that
 * the nodes are not pulled out from under processes still being killed. */
static pmix_status_t session_terminate(prte_session_t *session)
{
    int k, rc;
    prte_job_t *jdata;
    bool running = false;
    pmix_status_t ret = PMIX_SUCCESS;

    session->flags |= PRTE_SESSION_FLAG_TERMINATING;

    for (k = 0; k < session->jobs->size; k++) {
        jdata = (prte_job_t *) pmix_pointer_array_get_item(session->jobs, k);
        if (NULL == jdata) {
            continue;
        }
        running = true;
        /* a stopped process cannot act on a termination order, so let it run
         * again first - otherwise the kill only lands when something else
         * happens to continue it */
        if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_SUSPENDED)) {
            prte_plm.signal_job(jdata->nspace, SIGCONT);
            PRTE_FLAG_UNSET(jdata, PRTE_JOB_FLAG_SUSPENDED);
        }
        rc = prte_plm.terminate_job(jdata->nspace);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            ret = prte_pmix_convert_rc(rc);
        }
    }
    session->flags &= ~PRTE_SESSION_FLAG_PAUSED;

    if (!running) {
        /* nothing to wait for */
        reclaim_session(session);
    }
    return ret;
}

/* The session's time limit expired. Treat it exactly as a terminate from the
 * scheduler: that is what the limit means. */
static void session_timeout_cb(int fd, short args, void *cbdata)
{
    prte_session_t *session = (prte_session_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl: session %u time limit expired",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), session->session_id);

    /* the event is not persistent and has now fired, so drop our hold on it
     * before anything below can release the session */
    if (NULL != session->timer) {
        PMIX_RELEASE(session->timer);
        session->timer = NULL;
    }
    session->timeout.tv_sec = 0;

    session_terminate(session);
}

/* Add resources to an existing session, extend its time limit, or both. */
static pmix_status_t session_extend(prte_session_t *session, prte_sessctrl_t *ctl)
{
    int i, ret;
    bool grew = false;

    if (NULL != ctl->nodelists) {
        pmix_status_t prc = PMIX_SUCCESS;
        for (i = 0; NULL != ctl->nodelists[i]; i++) {
            ret = prte_ras_base_insert_node_string(ctl->nodelists[i], session);
            if (PRTE_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                prc = prte_pmix_convert_rc(ret);
                break;
            }
            grew = true;
        }
        if (PMIX_SUCCESS != prc) {
            /* Whatever was added is in the node pool and held by this session
             * already, so grow onto it before reporting the failure: unlike
             * an instantiation, there is no half-built session to unwind
             * here, and returning without the grow strands those nodes -
             * withheld from general use, with no daemon, usable by nobody. */
            if (grew) {
                prte_ras_base_activate_dvm_grow();
            }
            return prc;
        }
    }
    if (0 <= ctl->timelimit) {
        arm_session_timer(session, ctl->timelimit);
    }
    if (ctl->have_inheritance) {
        /* an extend may also revise what becomes of the nodes at the end */
        session->inheritance = ctl->inheritance;
    }
    if (!grew && 0 > ctl->timelimit && !ctl->have_inheritance) {
        /* an extend that extends nothing */
        return PMIX_ERR_BAD_PARAM;
    }
    if (grew) {
        /* launch daemons onto the newly added nodes */
        prte_ras_base_activate_dvm_grow();
    }
    return PMIX_SUCCESS;
}

/*****   INSTANTIATION   *****/

/* Build the job described by the request's PMIX_SESSION_APP/PMIX_SESSION_JOB
 * directives. Mirrors interim() in pmix_server_dyn.c, and in the same order:
 * the apps have to exist before the personality's default runtime options can
 * be resolved, and both have to precede the job info. Returns NULL on failure
 * with a PMIx status in *status. */
static prte_job_t *build_session_job(prte_sessctrl_t *ctl, prte_session_t *session,
                                     pmix_status_t *status)
{
    prte_job_t *jdata;
    prte_app_context_t *app;
    prte_rmaps_options_t options;
    prte_schizo_base_module_t *schizo;
    size_t i;
    int j, rc;

    *status = PMIX_SUCCESS;

    jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    PMIX_LOAD_PROCID(&jdata->originator, ctl->requestor.nspace, ctl->requestor.rank);

    if (NULL != ctl->personality) {
        jdata->personality = PMIx_Argv_split(ctl->personality->value.data.string, ',');
        jdata->schizo = (struct prte_schizo_base_module_t *)
            prte_schizo_base_detect_proxy(ctl->personality->value.data.string);
        pmix_server_cache_job_info(jdata, ctl->personality);
    } else {
        jdata->schizo = (struct prte_schizo_base_module_t *)
            prte_schizo_base_detect_proxy(NULL);
    }
    if (NULL == jdata->schizo) {
        /* no component claims the requested personality. Every later use of
         * jdata->schizo is an unchecked dereference, so refuse here rather
         * than let a bad personality string take down the DVM */
        prte_show_help("help-schizo-base.txt", "no-proxy", true, prte_tool_basename,
                       (NULL == ctl->personality) ? "NULL"
                                                  : ctl->personality->value.data.string);
        *status = PMIX_ERR_NOT_FOUND;
        goto error;
    }

    for (i = 0; i < ctl->napps; i++) {
        rc = prte_pmix_xfer_app(jdata, &ctl->apps[i]);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            *status = prte_pmix_convert_rc(rc);
            goto error;
        }
    }

    /* some runtime options are for the apps themselves, so this has to follow
     * the transfer above */
    memset(&options, 0, sizeof(prte_rmaps_options_t));
    options.stream = prte_rmaps_base_framework.framework_output;
    options.verbosity = 5;
    schizo = (prte_schizo_base_module_t *) jdata->schizo;
    rc = schizo->set_default_rto(jdata, &options);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        *status = prte_pmix_convert_rc(rc);
        goto error;
    }

    if (NULL != ctl->jobinfo) {
        rc = prte_pmix_xfer_job_info(jdata, ctl->jobinfo, ctl->njobinfo);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            *status = prte_pmix_convert_rc(rc);
            goto error;
        }
    }

    /* the job runs in the session we just created, whatever the job info may
     * have said - the session is the point of the request */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_SESSION_ID, PRTE_ATTR_GLOBAL,
                       &session->session_id, PMIX_UINT32);
    prte_remove_attribute(&jdata->attributes, PRTE_JOB_ALLOC_ID);
    prte_remove_attribute(&jdata->attributes, PRTE_JOB_REF_ID);
    prte_remove_attribute(&jdata->attributes, PRTE_JOB_SPAWN_TARGET);

    /* nodes the session holds are the ones this job may use */
    if (NULL != ctl->nodelists) {
        char *hosts = PMIx_Argv_join(ctl->nodelists, ',');
        if (NULL != hosts) {
            for (j = 0; j < jdata->apps->size; j++) {
                app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, j);
                if (NULL == app) {
                    continue;
                }
                if (!prte_get_attribute(&app->attributes, PRTE_APP_DASH_HOST, NULL,
                                        PMIX_STRING)) {
                    prte_set_attribute(&app->attributes, PRTE_APP_DASH_HOST,
                                       PRTE_ATTR_GLOBAL, hosts, PMIX_STRING);
                }
            }
            free(hosts);
        }
    }

    /* identifies the proc whose ownership and bookmark the launch path uses */
    prte_set_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, PRTE_ATTR_GLOBAL,
                       &jdata->originator, PMIX_PROC);
    PRTE_FLAG_SET(jdata, PRTE_JOB_FLAG_FORWARD_OUTPUT);

    return jdata;

error:
    PMIX_RELEASE(jdata);
    return NULL;
}

/* Build the answer for a completed request. Replaces req->info, taking care
 * not to strand an array the request already owned. */
static void set_response(prte_pmix_server_req_t *req, prte_sessctrl_t *ctl,
                         prte_session_t *session, const pmix_nspace_t nspace)
{
    pmix_info_t *rinfo;
    size_t n = 0, nresp = 0;

    if (NULL != ctl->ctrl_id) {
        nresp++;
    }
    if (NULL != session) {
        nresp++;                                     /* PMIX_SESSION_ID */
        if (NULL != session->alloc_refid) {
            nresp++;
        }
        if (NULL != session->user_refid) {
            nresp++;
        }
    }
    if (NULL != nspace && !PMIX_NSPACE_INVALID(nspace)) {
        nresp++;
    }

    PMIX_INFO_CREATE(rinfo, nresp);
    if (0 < nresp && NULL == rinfo) {
        /* nothing can be said, but the request must not be left holding the
         * caller's directives as its answer either - fall through with the
         * empty response the tail below installs */
        nresp = 0;
    }
    if (0 < nresp && NULL != ctl->ctrl_id) {
        PMIX_INFO_LOAD(&rinfo[n++], PMIX_SESSION_CTRL_ID, ctl->ctrl_id, PMIX_STRING);
    }
    if (0 < nresp && NULL != session) {
        PMIX_INFO_LOAD(&rinfo[n++], PMIX_SESSION_ID, &session->session_id, PMIX_UINT32);
        if (NULL != session->alloc_refid) {
            PMIX_INFO_LOAD(&rinfo[n++], PMIX_ALLOC_ID, session->alloc_refid, PMIX_STRING);
        }
        if (NULL != session->user_refid) {
            PMIX_INFO_LOAD(&rinfo[n++], PMIX_ALLOC_REQ_ID, session->user_refid, PMIX_STRING);
        }
    }
    if (0 < nresp && NULL != nspace && !PMIX_NSPACE_INVALID(nspace)) {
        PMIX_INFO_LOAD(&rinfo[n++], PMIX_NSPACE, (void *) nspace, PMIX_STRING);
    }

    /* The request's own array is normally borrowed from the PMIx caller and
     * needs no action, but one relayed from a peer owns its copy and would be
     * stranded here.
     *
     * That free is why this may run only ONCE per parsed request: every
     * string ctl holds - ctrl_id above all - points into that array, so a
     * second call would read the copy this one released.  It is also why the
     * replacement happens even when there is nothing to say: leaving the old
     * array in place answers the caller with its own directives echoed back
     * as results, and on the relayed path leaves an array we own attached to
     * a request whose answer has already gone out. */
    if (req->copy && NULL != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
    }
    req->info = rinfo;
    req->ninfo = nresp;
    req->copy = (NULL != rinfo);
}

/* Carries a response array to whatever eventually finishes with it. Plain
 * heap storage holding no PRRTE object, so the release callback may run on
 * either progress thread. */
typedef struct {
    pmix_info_t *info;
    size_t ninfo;
} prte_sessctrl_array_t;

static void free_response_array(void *cbdata)
{
    prte_sessctrl_array_t *ar = (prte_sessctrl_array_t *) cbdata;

    if (NULL == ar) {
        return;
    }
    PMIX_INFO_FREE(ar->info, ar->ninfo);
    free(ar);
}

/* Answer a request this daemon processed itself and retire it.
 *
 * A response array we built is DETACHED from the request and handed over with
 * its own release callback, because the consumer may not be finished with it
 * when this returns: the local-client path (PMIx's own callback) consumes it
 * synchronously, but the relayed path (send_alloc_resp) thread-shifts and packs
 * it later. Tying the array's lifetime to the release callback rather than to
 * the request is what lets one function serve both. */
static void answer_request(prte_pmix_server_req_t *req, pmix_status_t status)
{
    prte_sessctrl_array_t *ar = NULL;
    pmix_info_t *info = req->info;
    size_t ninfo = req->ninfo;

    if (NULL != req->infocbfunc) {
        if (req->copy && NULL != info) {
            ar = (prte_sessctrl_array_t *) malloc(sizeof(prte_sessctrl_array_t));
        }
        if (NULL != ar) {
            ar->info = info;
            ar->ninfo = ninfo;
            req->info = NULL;
            req->ninfo = 0;
            req->copy = false;
        }
        req->infocbfunc(status, info, ninfo, req->cbdata,
                        (NULL == ar) ? NULL : free_response_array, ar);
    }
    /* Retire the request. send_alloc_resp, the relayed path's callback, has
     * retained it for its own deferred half and will clear this slot again
     * from there - by which time the index may have been handed to somebody
     * else's request, so invalidate it and let that second clear no-op on the
     * negative index rather than blank out a stranger. */
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs,
                                req->local_index, NULL);
    req->local_index = -1;
    PMIX_RELEASE(req);
}

/* Completion of the job launched by a session instantiation. Runs on the
 * PRRTE progress thread (pmix_server_launch_resp is an RML receive). */
static void instantiate_spawn_cb(pmix_status_t status, pmix_nspace_t nspace,
                                 void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t *) cbdata;
    prte_session_t *session;
    prte_sessctrl_t ctl;

    session = prte_get_session_object(req->sessionID);

    if (PMIX_SUCCESS != status) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: job launch for session %u failed: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->sessionID,
                            PMIx_Error_string(status));
        /* the session exists only to run this job, so give the resources back
         * rather than leave them held by a session that will never run */
        if (NULL != session) {
            reclaim_session(session);
        }
        answer_request(req, status);
        return;
    }

    /* rebuild just enough of the parsed request to render the answer - the
     * original directive array is still ours until we call back */
    sessctrl_construct(&ctl);
    ctl.ctrl_id = req->key;
    set_response(req, &ctl, session, nspace);
    sessctrl_destruct(&ctl);

    answer_request(req, PMIX_SUCCESS);
}

/* Instantiate a new session: create it, give it its resources, and start
 * whatever jobs it was defined to run. */
static pmix_status_t session_instantiate(prte_pmix_server_req_t *req,
                                         prte_sessctrl_t *ctl)
{
    prte_session_t *session;
    prte_job_t *jdata = NULL;
    pmix_status_t rc = PMIX_SUCCESS;
    int i, ret;
    bool grew = false;

    if (UINT32_MAX == req->sessionID) {
        /* UINT32_MAX names every session, and is PRRTE's own id for the
         * default (unreserved) pool - neither can be instantiated */
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL != prte_get_session_object(req->sessionID)) {
        return PMIX_ERR_EXISTS;
    }
    if (NULL != ctl->apps && NULL == ctl->nodelists) {
        /* A session withholds the nodes named for it from general use, so one
         * created with no resource description holds nothing and no job in it
         * can be mapped. Refuse here rather than let the request succeed and
         * the job fail later with "no nodes are available", which says nothing
         * about the cause. The caller is required to describe the resources
         * (PMIX_SESSION_RESOURCES, or PMIX_ALLOC_NODE_LIST directly). */
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: instantiate of session %u names apps "
                            "but no resources for them to run on",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->sessionID);
        return PMIX_ERR_BAD_PARAM;
    }

    session = PMIX_NEW(prte_session_t);
    if (NULL == session) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_NOMEM;
    }
    session->session_id = req->sessionID;
    session->flags |= PRTE_SESSION_FLAG_DYNAMIC | PRTE_SESSION_FLAG_RESERVED |
                      PRTE_SESSION_FLAG_SCHEDULER;
    /* A session is DETACHED by default - i.e. its lifetime does not follow
     * the namespace that asked for it. That is the opposite of an allocation
     * reservation, deliberately: a reservation is nodes a tool took for its
     * own use, and dies with the tool, but a session is named, addressable by
     * anyone authorized, and destroyed by an explicit terminate, by its own
     * time limit, or - when it was created to run apps - when those apps
     * finish. Following the requester instead makes the API unusable for the
     * thing it exists to do: the requester is a tool, its namespace lasts
     * only as long as the request, and the session would be reclaimed out
     * from under its own jobs the moment the caller disconnected.
     * PMIX_SESSION_SEP given as FALSE opts back into requester-lifetime. */
    if (!(ctl->have_sep && !ctl->sep)) {
        session->flags |= PRTE_SESSION_FLAG_DETACHED;
    }
    if (NULL != ctl->apps) {
        /* the session was defined to run these jobs, so it is finished when
         * they are. An instantiation that named no apps is a standing
         * reservation and persists until explicitly terminated. */
        session->flags |= PRTE_SESSION_FLAG_AUTO_COMPLETE;
    }
    if (NULL != ctl->alloc_refid) {
        session->alloc_refid = strdup(ctl->alloc_refid);
    } else {
        pmix_asprintf(&session->alloc_refid, "%s.%u", ctl->requestor.nspace,
                      session->session_id);
    }
    if (NULL != ctl->user_refid) {
        session->user_refid = strdup(ctl->user_refid);
    }
    session->inheritance = ctl->inheritance;
    PMIX_LOAD_NSPACE(session->owner, ctl->requestor.nspace);
    PMIX_XFER_PROCID(&session->requestor, &ctl->requestor);
    prte_session_add_owner(session, ctl->requestor.nspace);
    if (ctl->have_uid) {
        session->owner_uid = ctl->uid;
    } else {
        prte_job_t *reqjob = prte_get_job_data_object(ctl->requestor.nspace);
        if (NULL != reqjob) {
            session->owner_uid = reqjob->uid;
        }
    }

    ret = prte_set_session_object(session);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_RELEASE(session);
        return prte_pmix_convert_rc(ret);
    }

    /* give the session its nodes. They join the global pool and are then
     * withheld from general use by their session backpointer. */
    if (NULL != ctl->nodelists) {
        for (i = 0; NULL != ctl->nodelists[i]; i++) {
            ret = prte_ras_base_insert_node_string(ctl->nodelists[i], session);
            if (PRTE_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                rc = prte_pmix_convert_rc(ret);
                goto error;
            }
            grew = true;
        }
    }

    if (0 < ctl->timelimit) {
        arm_session_timer(session, ctl->timelimit);
    }

    if (NULL != ctl->apps) {
        jdata = build_session_job(ctl, session, &rc);
        if (NULL == jdata) {
            goto error;
        }
    }

    if (grew) {
        /* Extend the DVM across the new nodes.  Mark it not-ready first: the
         * job built below is submitted through the ordinary launch path,
         * which parks a job in prte_cache while the DVM is not ready and
         * releases it at the VM_READY re-entry that ends the grow.  That is
         * exactly the ordering an instantiation needs - its apps are meant to
         * run on the nodes this call is about to bring in - and without the
         * mark the job goes straight to the mapper, which sees nodes that as
         * yet have no daemon.  Every other producer of a grow that submits
         * work behind it does the same (prte_ras_base_add_hosts,
         * prte_ras_base_activate_hosts). */
        prte_dvm_ready = false;
        prte_ras_base_activate_dvm_grow();
    }

    if (NULL == jdata) {
        /* a standing reservation - nothing to wait for */
        return PMIX_SUCCESS;
    }

    /* the answer carries the launched namespace, so it has to wait for the
     * launch. Stash the request id for the completion to echo back. */
    if (NULL != ctl->ctrl_id) {
        if (NULL != req->key) {
            free(req->key);
        }
        req->key = strdup(ctl->ctrl_id);
    }
    prte_pmix_server_launch_job(jdata, instantiate_spawn_cb, req);
    return PMIX_OPERATION_IN_PROGRESS;

error:
    /* unwind the half-built session, returning any nodes it took */
    prte_ras_base_teardown_reservation(session, false);
    PMIX_RELEASE(session);
    return rc;
}

/*****   DISPATCH   *****/

/* Apply an operation to one session. Returns a PMIx status, or
 * PMIX_OPERATION_IN_PROGRESS if it has taken over answering the request. */
static pmix_status_t apply_to_session(prte_pmix_server_req_t *req,
                                      prte_sessctrl_t *ctl,
                                      prte_session_t *session,
                                      bool respond)
{
    pmix_status_t rc = PMIX_SUCCESS;

    if (!AUTHORIZED(session, &ctl->requestor)) {
        return PMIX_ERR_NO_PERMISSIONS;
    }

    /* a modifier may accompany any operation, and may stand alone */
    if (ctl->have_sep) {
        if (ctl->sep) {
            session->flags |= PRTE_SESSION_FLAG_DETACHED;
        } else {
            session->flags &= ~PRTE_SESSION_FLAG_DETACHED;
        }
    }

    switch (ctl->op) {
    case PRTE_SESSCTRL_MODIFY:
        break;

    case PRTE_SESSCTRL_PAUSE:
        /* PRRTE stops the processes where they stand. Their memory and their
         * slots are retained - it has no checkpoint facility with which to
         * vacate and later reinstate them. */
        rc = session_signal(session, NULL, SIGSTOP, true, true);
        if (PMIX_SUCCESS == rc) {
            session->flags |= PRTE_SESSION_FLAG_PAUSED;
        }
        break;

    case PRTE_SESSCTRL_RESUME:
        rc = session_signal(session, NULL, SIGCONT, true, false);
        if (PMIX_SUCCESS == rc) {
            session->flags &= ~PRTE_SESSION_FLAG_PAUSED;
        }
        break;

    case PRTE_SESSCTRL_PREEMPT:
        /* Preemption is suspension-based: the named jobs (all of them, absent
         * a PMIX_NSPACE qualifier) are stopped and hold their resources. The
         * attribute speaks of recovering those resources, which would require
         * checkpointing the jobs out and restoring them later - PRRTE has no
         * such facility, and terminating them instead would make the paired
         * PMIX_SESSION_RESTORE impossible to honor. A scheduler that wants
         * the nodes back should terminate the session. */
        rc = session_signal(session, ctl->nspaces, SIGSTOP, true, true);
        break;

    case PRTE_SESSCTRL_RESTORE:
        rc = session_signal(session, ctl->nspaces, SIGCONT, true, false);
        break;

    case PRTE_SESSCTRL_SIGNAL:
        rc = session_signal(session, ctl->nspaces, ctl->sigvalue, false, false);
        break;

    case PRTE_SESSCTRL_TERMINATE:
        rc = session_terminate(session);
        /* the session is going away - do not report it back */
        session = NULL;
        break;

    case PRTE_SESSCTRL_EXTEND:
        rc = session_extend(session, ctl);
        break;

    case PRTE_SESSCTRL_COMPLETE:
        /* PMIX_SESSION_COMPLETE is what an RTE says to its scheduler, not
         * something a scheduler says to its RTE: PRRTE is the only party that
         * knows when the jobs have finished and the nodes are free. Accepting
         * it here would mean discarding a session while its processes may
         * still be running. */
        rc = PMIX_ERR_NOT_SUPPORTED;
        break;

    case PRTE_SESSCTRL_INSTANTIATE:
        /* the session already exists */
        rc = PMIX_ERR_EXISTS;
        break;

    default:
        rc = PMIX_ERR_BAD_PARAM;
        break;
    }

    if (respond && PMIX_SUCCESS == rc) {
        set_response(req, ctl, session, NULL);
    }
    return rc;
}

/* Apply an operation to every session this requestor controls - what a
 * sessionID of UINT32_MAX asks for. The default (unreserved) pool is not a
 * session anyone instantiated, so it is never a target. */
static pmix_status_t apply_to_all(prte_pmix_server_req_t *req,
                                  prte_sessctrl_t *ctl)
{
    prte_session_t *session;
    pmix_status_t rc, ret = PMIX_ERR_NOT_FOUND;
    int i;

    if (NULL == prte_sessions) {
        return PMIX_ERR_NOT_FOUND;
    }
    /* Terminating a session can remove it from the array from under this
     * walk.  That is safe as written: removal only NULLs the slot (the object
     * itself survives for as long as we hold it), and the array never
     * shrinks, so re-reading ->size each time is correct. */
    for (i = 0; i < prte_sessions->size; i++) {
        session = (prte_session_t *) pmix_pointer_array_get_item(prte_sessions, i);
        if (NULL == session || session == prte_default_session) {
            continue;
        }
        if (!AUTHORIZED(session, &ctl->requestor)) {
            continue;
        }
        /* Do NOT let the per-session call build the answer.  It may run many
         * times here, and set_response() may run only once per parsed
         * request: it frees the array ctl's strings point into, so a second
         * call reads the copy the first released - and the answer it would
         * leave behind names whichever session happened to be served last,
         * which is not what a request addressed to all of them asked about. */
        rc = apply_to_session(req, ctl, session, false);
        if (PMIX_OPERATION_IN_PROGRESS == rc) {
            /* nothing in the all-sessions set can defer its answer: only an
             * instantiation does that, and instantiate is rejected above */
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        if (PMIX_SUCCESS != rc) {
            ret = rc;           /* report the last failure */
        } else if (PMIX_ERR_NOT_FOUND == ret) {
            ret = PMIX_SUCCESS; /* at least one session was served */
        }
    }
    if (PMIX_SUCCESS == ret) {
        /* no session is named: the request was about all of them */
        set_response(req, ctl, NULL, NULL);
    }
    return ret;
}

/* Process a session control directive addressed to this DVM.
 *
 * Returns a pmix_status_t - NOT a PRRTE return code. The result is handed to
 * req->infocbfunc, which is a PMIx callback, so every exit from here must
 * already be in PMIx's numbering. A return of PMIX_OPERATION_IN_PROGRESS means
 * this function has taken over answering the request; on every other return
 * the answer, if any, is left on the request as req->info and the caller
 * delivers it. Answering here as well would deliver the response twice and
 * release the request twice. */
static pmix_status_t process_directive(prte_pmix_server_req_t *req)
{
    prte_sessctrl_t ctl;
    prte_session_t *session;
    pmix_status_t rc;

    sessctrl_construct(&ctl);

    rc = parse_directives(req, &ctl);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl: op %d on session %u for %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) ctl.op,
                        req->sessionID, PRTE_NAME_PRINT(&ctl.requestor));

    if (PRTE_SESSCTRL_NONE == ctl.op) {
        /* nothing was asked of us */
        rc = PMIX_ERR_BAD_PARAM;
        goto done;
    }

    if (PRTE_SESSCTRL_INSTANTIATE == ctl.op) {
        rc = session_instantiate(req, &ctl);
        if (PMIX_SUCCESS == rc) {
            session = prte_get_session_object(req->sessionID);
            set_response(req, &ctl, session, NULL);
        }
        goto done;
    }

    if (UINT32_MAX == req->sessionID) {
        rc = apply_to_all(req, &ctl);
        goto done;
    }

    session = prte_get_session_object(req->sessionID);
    if (NULL == session || session == prte_default_session) {
        /* the default pool is not a session anyone may operate on - pausing
         * or terminating "the session" would take the whole DVM with it */
        rc = PMIX_ERR_NOT_FOUND;
        goto done;
    }

    rc = apply_to_session(req, &ctl, session, true);

done:
    sessctrl_destruct(&ctl);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_IN_PROGRESS != rc) {
        /* Nothing to report but the status, and what the request is still
         * carrying is the array it arrived with - the caller's own
         * directives, which must not go back looking like results.  (An
         * in-progress request keeps it: the deferred half replaces it when
         * it builds the real answer.) */
        if (req->copy && NULL != req->info) {
            PMIX_INFO_FREE(req->info, req->ninfo);
        }
        req->info = NULL;
        req->ninfo = 0;
        req->copy = false;
    }
    return rc;
}

/* Execute a session control directive against this DVM's own sessions and
 * answer the request. The two callers - pass_request, for a request that
 * arrived at this daemon directly, and pmix_server_sched, for one a peer
 * daemon relayed here - deliver their answers through different callbacks,
 * which is why the completion is done here rather than left to them.
 *
 * MUST be called on the PRRTE progress thread, on the DVM master. */
void prte_pmix_server_session_ctrl_local(prte_pmix_server_req_t *req)
{
    pmix_status_t rc;

    rc = process_directive(req);
    if (PMIX_OPERATION_IN_PROGRESS == rc) {
        /* the operation will answer for itself when it completes */
        return;
    }
    answer_request(req, rc);
}

/*****   SESSION COMPLETION REPORTING   *****/

/* PMIx invokes this on its own progress thread once it is done with the
 * notification. It touches nothing but the array we allocated for the call. */
static void session_complete_cbfunc(pmix_status_t status,
                                    pmix_info_t *info, size_t ninfo,
                                    void *cbdata,
                                    pmix_release_cbfunc_t rel, void *relcbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(status, info, ninfo);

    if (NULL != rel) {
        rel(relcbdata);
    }
    free_response_array(cbdata);
}

void prte_pmix_server_session_complete(prte_session_t *session)
{
    prte_sessctrl_array_t *ar;
    pmix_info_t *info;
    size_t n, k, ninfo, nresults;
    pmix_status_t rc;

    if (NULL == session || session == prte_default_session) {
        return;
    }
    if (!(session->flags & PRTE_SESSION_FLAG_SCHEDULER)) {
        /* nobody asked us for this session, so nobody is waiting to hear that
         * it is done */
        return;
    }
    /* report it exactly once */
    session->flags &= ~PRTE_SESSION_FLAG_SCHEDULER;

    rc = prte_pmix_set_scheduler();
    if (PMIX_SUCCESS != rc) {
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: no scheduler to report completion "
                            "of session %u to", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            session->session_id);
        return;
    }

    nresults = session->nresults;
    /* the completion directive, the session id, the per-job results, and the
     * allocation id if the session carries one */
    ninfo = 2 + nresults + ((NULL == session->alloc_refid) ? 0 : 1);
    PMIX_INFO_CREATE(info, ninfo);
    if (NULL == info) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_SESSION_COMPLETE, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SESSION_ID, &session->session_id, PMIX_UINT32);
    n = 2;
    if (NULL != session->alloc_refid) {
        PMIX_INFO_LOAD(&info[n++], PMIX_ALLOC_ID, session->alloc_refid, PMIX_STRING);
    }
    for (k = 0; k < nresults; k++) {
        PMIX_INFO_XFER(&info[n++], &session->results[k]);
    }

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl: reporting completion of session %u "
                        "with %" PRIsize_t " job results",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), session->session_id,
                        nresults);

    ar = (prte_sessctrl_array_t *) malloc(sizeof(prte_sessctrl_array_t));
    if (NULL == ar) {
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    ar->info = info;
    ar->ninfo = ninfo;

    /* the non-blocking form: the blocking one would park this - the PRRTE
     * progress - thread inside PMIx while it waits for a reply */
    rc = PMIx_Session_control(session->session_id, info, ninfo,
                              session_complete_cbfunc, ar);
    if (PMIX_SUCCESS != rc) {
        /* PMIx does not invoke the callback when the call itself fails */
        if (PMIX_OPERATION_SUCCEEDED != rc) {
            PMIX_ERROR_LOG(rc);
        }
        free_response_array(ar);
    }
}

void prte_pmix_server_session_job_terminated(prte_session_t *session,
                                             prte_job_t *jdata)
{
    pmix_info_t *results;
    pmix_data_array_t darray;
    pmix_info_t jinfo[2];
    pmix_status_t jstatus;
    int k;

    if (NULL == session || session == prte_default_session || NULL == jdata) {
        return;
    }

    /* Record what became of the job. The job object does not survive to the
     * end of the session, and the completion notification is required to carry
     * the status of every job the session ran. */
    if (session->flags & PRTE_SESSION_FLAG_SCHEDULER) {
        jstatus = prte_pmix_convert_job_state_to_error(jdata->state);
        PMIX_INFO_LOAD(&jinfo[0], PMIX_NSPACE, jdata->nspace, PMIX_STRING);
        PMIX_INFO_LOAD(&jinfo[1], PMIX_JOB_TERM_STATUS, &jstatus, PMIX_STATUS);
        darray.type = PMIX_INFO;
        darray.size = 2;
        darray.array = jinfo;

        results = (pmix_info_t *) realloc(session->results,
                                          (session->nresults + 1) * sizeof(pmix_info_t));
        if (NULL != results) {
            session->results = results;
            PMIX_INFO_CONSTRUCT(&session->results[session->nresults]);
            PMIX_INFO_LOAD(&session->results[session->nresults],
                           PMIX_JOB_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            session->nresults++;
        }
        PMIX_INFO_DESTRUCT(&jinfo[0]);
        PMIX_INFO_DESTRUCT(&jinfo[1]);
    }

    /* a session that exists to run its jobs, or one being torn down, is
     * finished once the last of them has gone */
    if (!(session->flags & (PRTE_SESSION_FLAG_AUTO_COMPLETE |
                            PRTE_SESSION_FLAG_TERMINATING))) {
        return;
    }
    for (k = 0; k < session->jobs->size; k++) {
        if (NULL != pmix_pointer_array_get_item(session->jobs, k)) {
            return;     /* still running */
        }
    }

    reclaim_session(session);
}

/*****   REQUEST PLUMBING   *****/

/* Callbacks to process a session control answer from the scheduler
 * and pass on any results to the requesting client
 */
static void passthru(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL != req->infocbfunc) {
        // call the requestor's callback with the returned info
        req->infocbfunc(req->status, req->info, req->ninfo, req->cbdata, req->rlcbfunc, req->rlcbdata);
    } else if (NULL != req->rlcbfunc) {
        // no one to answer, but the result array still has to go back
        req->rlcbfunc(req->rlcbdata);
    }
    // cleanup our request
    pmix_pointer_array_set_item(&prte_pmix_server_globals.local_reqs, req->local_index, NULL);
    PMIX_RELEASE(req);
}

/* Caddy carrying a session-control result from the PMIx progress thread over
 * to the PRRTE progress thread - the same shape, and for the same reason, as
 * prte_alloc_resp_caddy_t in pmix_server.c.  A prte_pmix_server_req_t is a
 * PRRTE object owned by the PRRTE progress thread, so the PMIx-thread callback
 * must only capture the result; recording it on the request - and above all
 * freeing the array the request was carrying - has to happen in the shifted
 * handler. */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_pmix_server_req_t *req;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
    pmix_release_cbfunc_t rel;
    void *relcbdata;
} prte_sessctrl_resp_caddy_t;
static void srcon(prte_sessctrl_resp_caddy_t *p)
{
    p->req = NULL;
    p->status = PMIX_SUCCESS;
    p->info = NULL;
    p->ninfo = 0;
    p->rel = NULL;
    p->relcbdata = NULL;
}
static void srdes(prte_sessctrl_resp_caddy_t *p)
{
    if (NULL != p->req) {
        PMIX_RELEASE(p->req);
    }
}
static PMIX_CLASS_INSTANCE(prte_sessctrl_resp_caddy_t, pmix_object_t, srcon, srdes);

/* record the result on the request and answer - runs on the PRRTE
 * progress thread */
static void _sessctrl_resp(int sd, short args, void *cbdata)
{
    prte_sessctrl_resp_caddy_t *cd = (prte_sessctrl_resp_caddy_t*)cbdata;
    prte_pmix_server_req_t *req = cd->req;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* now safe to touch the request. Beware that a caller may hand the
     * request's OWN info array back as the result - do not free it in
     * that case */
    req->status = cd->status;
    if (req->copy && NULL != req->info && cd->info != req->info) {
        PMIX_INFO_FREE(req->info, req->ninfo);
        req->copy = false;
    }
    req->info = cd->info;
    req->ninfo = cd->ninfo;
    req->rlcbfunc = cd->rel;
    req->rlcbdata = cd->relcbdata;

    passthru(0, 0, req);
    /* passthru released the request's array-slot reference; drop ours */
    PMIX_RELEASE(cd);
}

static void infocbfunc(pmix_status_t status,
                       pmix_info_t *info, size_t ninfo,
                       void *cbdata,
                       pmix_release_cbfunc_t rel, void *relcbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    prte_sessctrl_resp_caddy_t *cd;

    /* GOLDEN RULE: this can be invoked on the PMIx progress thread, so
     * capture the result and post it - touch no PRRTE state here */
    cd = PMIX_NEW(prte_sessctrl_resp_caddy_t);
    PMIX_RETAIN(req);
    cd->req = req;
    cd->status = status;
    cd->info = info;
    cd->ninfo = ninfo;
    cd->rel = rel;
    cd->relcbdata = relcbdata;

    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, _sessctrl_resp);
}

static void pass_request(int sd, short args, void *cbdata)
{
    prte_pmix_server_req_t *req = (prte_pmix_server_req_t*)cbdata;
    pmix_status_t rc;
    size_t n;
    pmix_info_t *xfer;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* add this request to our local request tracker array */
    req->local_index = pmix_pointer_array_add(&prte_pmix_server_globals.local_reqs, req);

    if (!PRTE_PROC_IS_MASTER) {
        /* if we are not the DVM master, then we have to send
         * this request to the master for processing */
        pmix_output_verbose(2, prte_pmix_server_globals.output,
                            "%s session ctrl: relaying to the master as local req %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), req->local_index);
        rc = prte_server_send_request(PRTE_PMIX_SESSION_CTRL, req);
        if (PMIX_SUCCESS != rc) {
            goto callback;
        }
        return;
    }

    /* if we are the DVM master, then handle this ourselves - start
     * by seeing whether a scheduler is available to us. This is allowed
     * to fail: a DVM running without a scheduler is its own authority
     * over its sessions. */
    rc = prte_pmix_set_scheduler();

    /* A directive from the scheduler is a directive to this DVM and is
     * executed here. So is one from anybody else when there is no scheduler
     * to defer to - otherwise a standalone DVM could not control its own
     * sessions at all. Everything else is passed up to the scheduler, which
     * owns the decision and will call back down to us to carry it out. */
    if (PMIX_SUCCESS != rc ||
        PMIX_CHECK_PROCID(&prte_pmix_server_globals.scheduler, &req->tproc)) {
        prte_pmix_server_session_ctrl_local(req);
        return;
    } else {
        // we need to pass the request on to the scheduler
        // need to add the requestor's ID to the info array
        PMIX_INFO_CREATE(xfer, req->ninfo + 1);
        for (n=0; n < req->ninfo; n++) {
            PMIX_INFO_XFER(&xfer[n], &req->info[n]);
        }
        PMIX_INFO_LOAD(&xfer[req->ninfo], PMIX_REQUESTOR, &req->tproc, PMIX_PROC);
        // the current req object points to the caller's info array, so leave it alone
        req->copy = true;
        req->info = xfer;
        req->ninfo++;
        rc = PMIx_Session_control(req->sessionID, req->info, req->ninfo,
                                  infocbfunc, req);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto callback;
        }
        return;
    }

callback:
    /* this section gets executed solely upon an error or if this was a
     * directive to us from the scheduler */
    answer_request(req, rc);
}

/* this is the upcall from the PMIx server for the session
 * control support. Since we are going to touch global structures
 * (e.g., the session tracker pointer array), we have to threadshift
 * this request into our own internal progress thread. Note that the
 * session control directive could have come to this host from the
 * scheduler, or a tool, or even an application process. */
pmix_status_t pmix_server_session_ctrl_fn(const pmix_proc_t *requestor,
                                          uint32_t sessionID,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_server_req_t *req;

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s session ctrl upcalled on behalf of proc %s:%u with %" PRIsize_t " directives",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), requestor->nspace, requestor->rank, ndirs);

    /* create a request tracker for this operation */
    req = PMIX_NEW(prte_pmix_server_req_t);
    pmix_asprintf(&req->operation, "SESSIONCTRL: %u", sessionID);
    PMIX_PROC_LOAD(&req->tproc, requestor->nspace, requestor->rank);
    req->sessionID = sessionID;
    req->info = (pmix_info_t *) directives;
    req->ninfo = ndirs;
    req->infocbfunc = cbfunc;
    req->cbdata = cbdata;

    prte_event_set(prte_event_base, &req->ev, -1, PRTE_EV_WRITE, pass_request, req);
    PMIX_POST_OBJECT(req);
    prte_event_active(&req->ev, PRTE_EV_WRITE, 1);
    return PMIX_SUCCESS;
}
