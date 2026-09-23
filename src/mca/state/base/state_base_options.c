/*
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#if HAVE_UNISTD_H
#    include <unistd.h>
#endif
#if HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <pmix.h>
#include <pmix_server.h>

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/odls/base/base.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/schizo/base/base.h"
#include "src/rml/rml.h"
#include "src/prted/pmix/pmix_server_internal.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/prte_cmd_line.h"
#include "src/util/session_dir.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/state/base/base.h"

/* Record what a directive on the command line or in a PMIX_RUNTIME_OPTIONS
 * spec asked for.  BOTH truths are stored: the user writing "opt=false" has
 * answered the question, and the defaults pass above must leave that answer
 * alone rather than mistake it for the key nobody mentioned.  This used to
 * remove the attribute for a false value, because every reader tested
 * presence and a stored false read as enabled; readers use
 * PRTE_ATTR_IS_TRUE now, which is false for FALSE and for NOT_SET alike. */
static void set_bool_option(prte_job_t *jdata, prte_attribute_key_t key, bool flag)
{
    prte_set_bool_attribute(&jdata->attributes, key, PRTE_ATTR_GLOBAL, flag);
}

/* Does SPEC - a runtime-option string as it came off a command line - ask
 * that the exit status of child jobs be reported separately?
 *
 * The DVM answers this for itself from the attribute the walk below records,
 * but a launcher driving a PERSISTENT DVM has to answer it for its own exit
 * status, in its own process, with no job object anywhere near it.  The
 * reader lives here beside the writer so that the directive has one spelling
 * and one set of truth rules; a private copy in the tool would go stale the
 * first time either changed.
 *
 * Anything malformed reads as "no".  This is not where a bad command line is
 * diagnosed - the DVM does that, with the message and the failure - and
 * answering twice would only mean saying it twice. */
bool prte_state_base_report_child_sep(const char *spec)
{
    char **options, *ptr;
    pmix_value_t value;
    bool flag = false;
    int n, tag;

    if (NULL == spec) {
        return false;
    }
    options = PMIx_Argv_split(spec, ',');
    for (n = 0; NULL != options && NULL != options[n]; n++) {
        if (PMIX_CLI_MATCH_FOUND != pmix_cli_match(options[n], prte_cli_rtos_directives, &tag) ||
            PRTE_RTOS_REPORT_CHILD_SEP != tag) {
            continue;
        }
        ptr = PMIX_CLI_QUALIFIER_VALUE(options[n]);
        /* the same pair the walk below uses, so a value this says "true" to
         * is exactly one that sets the attribute there */
        PMIX_VALUE_LOAD(&value, ptr, PMIX_STRING);
        flag = PMIX_CHECK_TRUE(&value);
        PMIX_VALUE_DESTRUCT(&value);
    }
    PMIx_Argv_free(options);
    return flag;
}

/* this function is called if pmix_server_dyn receives a
 * PMIX_RUNTIME_OPTIONS info struct */
int prte_state_base_set_runtime_options(prte_job_t *jdata, char *spec)
{
    char **options, *ptr, *bkpt;
    int n, k, tm, rc, tag;
    int32_t i32;
    bool flag;
    prte_job_t *djob;
    prte_app_context_t *app;
    pmix_info_t info;
    pmix_value_t value;

    if (NULL == spec) {
        /* Fill in the options nobody has expressed a view on.
         *
         * A boolean attribute is three-state (see attr.h), which is what
         * makes this possible: an option the spawn explicitly set to FALSE
         * is left exactly as it was given, while one that is NOT_SET is
         * nobody's answer and takes this framework's default. Readers test
         * with PRTE_ATTR_IS_TRUE, so an explicit false reads as off.
         *
         * This used to depend on prte_set_attribute() APPENDING a false
         * boolean, so that "the user said no" could be told from "nobody
         * said" by the value, and then removed the false one to leave
         * something the presence tests could read. Both halves of that are
         * gone. */
        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_ERROR_NONZERO_EXIT)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_ERROR_NONZERO_EXIT, PRTE_ATTR_GLOBAL,
                                    prte_state_base.error_non_zero_exit);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_SHOW_PROGRESS)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_SHOW_PROGRESS, PRTE_ATTR_GLOBAL,
                                    prte_state_base.show_launch_progress);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_RECOVERABLE, PRTE_ATTR_GLOBAL,
                                    prte_state_base.recoverable);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_CONTINUOUS, PRTE_ATTR_GLOBAL,
                                    prte_state_base.continuous);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_NOTIFY_ERRORS)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_NOTIFY_ERRORS, PRTE_ATTR_GLOBAL,
                                    prte_state_base.notifyerrors);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_AUTORESTART)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_AUTORESTART, PRTE_ATTR_GLOBAL,
                                    prte_state_base.autorestart);
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_REPORT_CHILD_SEP, PRTE_ATTR_GLOBAL,
                                    prte_report_child_jobs_separately);
        }

        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_EXEC_AGENT, NULL, PMIX_STRING)) {
            if (NULL != prte_odls_globals.exec_agent) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_EXEC_AGENT,
                                   PRTE_ATTR_GLOBAL,
                                   prte_odls_globals.exec_agent, PMIX_STRING);
            }
        }

        if (PRTE_ATTR_NOT_SET == prte_get_bool_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT)) {
            /* nobody has said - take the default */
            prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, PRTE_ATTR_GLOBAL,
                                    prte_fwd_environment);
        }

        /* check the apps for max restarts */
        if (0 < prte_state_base.max_restarts) {
            for (n = 0; n < jdata->apps->size; n++) {
                app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, n);
                if (NULL == app) {
                    continue;
                }
                if (!prte_get_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, NULL, PMIX_INT32)) {
                    prte_set_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, PRTE_ATTR_GLOBAL,
                                       &prte_state_base.max_restarts, PMIX_INT32);
                }
            }
        }

    } else {
        options = PMIx_Argv_split(spec, ',');
        for (n=0; NULL != options && NULL != options[n]; n++) {
            /* Matched against the whole vocabulary, which also says which
             * directives carry a value - so a value missing from one that
             * needs it ("timeout", "timeout="), or promised by an '=' and
             * not given, is refused here with the directive named. */
            rc = prte_cli_match(PRTE_JOB_NSPACE(jdata), "--rtos", options[n],
                                prte_cli_rtos_directives, &tag);
            if (PRTE_ERR_NOT_FOUND == rc) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-prte-rmaps-base.txt", "unrecognized-policy", true,
                               "runtime options", spec);
                PMIx_Argv_free(options);
                return PRTE_ERR_SILENT;
            } else if (PRTE_SUCCESS != rc) {
                PMIx_Argv_free(options);
                return rc;
            }
            ptr = PMIX_CLI_QUALIFIER_VALUE(options[n]);
            PMIX_VALUE_LOAD(&value, ptr, PMIX_STRING); // just in case we need to evaluate a bool
            /* Every directive below except the handful that carry a value of
             * their own is a BOOLEAN, read with PMIX_CHECK_TRUE - which
             * reports anything that is neither true nor false as FALSE.  So
             * "donotlaunch=maybe" would quietly launch.  Refuse the value
             * here instead, where it is still the user's command line and
             * not a policy nobody asked for. */
            if (NULL != ptr &&
                !prte_schizo_base_directive_is_valued(prte_cli_name(prte_cli_rtos_directives, tag)) &&
                PRTE_RTOS_STOP_IN_APP != tag &&
                PRTE_SUCCESS != prte_cli_bool_value(ptr, &flag)) {
                prte_show_help(PRTE_JOB_NSPACE(jdata), "help-schizo-base.txt", "non-boolean-value", true,
                               PRTE_CLI_RTOS, prte_cli_name(prte_cli_rtos_directives, tag), ptr);
                PMIX_VALUE_DESTRUCT(&value);
                PMIx_Argv_free(options);
                return PRTE_ERR_SILENT;
            }
            /* act on the directive */
            if (PRTE_RTOS_ERROR_NZ == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_ERROR_NONZERO_EXIT, flag);

            } else if (PRTE_RTOS_NOLAUNCH == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_DO_NOT_LAUNCH, flag);
                /* if we are not in a persistent DVM, then make sure we also
                 * apply this to the daemons */
                if (!prte_persistent) {
                    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
                    if (NULL != djob) {
                        set_bool_option(djob, PRTE_JOB_DO_NOT_LAUNCH, flag);
                    }
                }

            } else if (PRTE_RTOS_NOSPAWN == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_DO_NOT_SPAWN, flag);
                /* if we are not in a persistent DVM, then make sure we also
                 * apply this to the daemons */
                if (!prte_persistent) {
                    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
                    if (NULL != djob) {
                        set_bool_option(djob, PRTE_JOB_DO_NOT_SPAWN, flag);
                    }
                }

            } else if (PRTE_RTOS_SHOW_PROGRESS == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_SHOW_PROGRESS, flag);

            } else if (PRTE_RTOS_NOTIFY_ERRORS == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_NOTIFY_ERRORS, flag);

            } else if (PRTE_RTOS_RECOVERABLE == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_RECOVERABLE, flag);

            } else if (PRTE_RTOS_AUTORESTART == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_AUTORESTART, flag);

            } else if (PRTE_RTOS_CONTINUOUS == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_CONTINUOUS, flag);

            } else if (PRTE_RTOS_MAX_RESTARTS == tag) {
                /* the vocabulary requires the value, so it is here */
                i32 = strtol(ptr, NULL, 10);
                /* 'k', not 'n' - 'n' indexes the option list we are walking */
                for (k = 0; k < jdata->apps->size; k++) {
                    app = (prte_app_context_t *) pmix_pointer_array_get_item(jdata->apps, k);
                    if (NULL == app) {
                        continue;
                    }
                    prte_set_attribute(&app->attributes, PRTE_APP_MAX_RESTARTS, PRTE_ATTR_GLOBAL,
                                       &i32, PMIX_INT32);
                }

            } else if (PRTE_RTOS_EXEC_AGENT == tag) {
                prte_set_attribute(&jdata->attributes, PRTE_JOB_EXEC_AGENT, PRTE_ATTR_GLOBAL,
                                   ptr, PMIX_STRING);

            } else if (PRTE_RTOS_DEFAULT_EXEC_AGENT == tag) {
                prte_remove_attribute(&jdata->attributes, PRTE_JOB_EXEC_AGENT);

            } else if (PRTE_RTOS_STOP_ON_EXEC == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                if (flag) {
                    prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC, PRTE_ATTR_GLOBAL, true);
                } else {
                    prte_remove_attribute(&jdata->attributes, PRTE_JOB_STOP_ON_EXEC);
                }

            } else if (PRTE_RTOS_STOP_IN_INIT == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                if (flag) {
                    prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT, PRTE_ATTR_GLOBAL, true);
                    /* also must add to job-level cache */
                    PMIX_INFO_LOAD(&info, PMIX_DEBUG_STOP_IN_INIT, NULL, PMIX_BOOL);
                    pmix_server_cache_job_info(jdata, &info);
                } else {
                    prte_remove_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_INIT);
                }

            } else if (PRTE_RTOS_STOP_IN_APP == tag) {
                /* this is the one hybrid directive: written bare or with a
                 * truth value it asserts the boolean "stop wherever the
                 * application chooses to stop", and written with anything
                 * else that text is the string ID of the ONE breakpoint the
                 * application is to stop at.  This is why the strict-boolean
                 * refusal above steps around this directive.  A breakpoint
                 * therefore cannot be named with any spelling of a truth
                 * value - "true" and "false" mean what they always mean. */
                if (PRTE_SUCCESS == prte_cli_bool_value(ptr, &flag)) {
                    bkpt = NULL;
                } else {
                    flag = true;
                    bkpt = ptr;
                }
                if (flag) {
                    prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP, PRTE_ATTR_GLOBAL, true);
                    /* also must add to job-level cache */
                    PMIX_INFO_LOAD(&info, PMIX_DEBUG_STOP_IN_APP, NULL, PMIX_BOOL);
                    pmix_server_cache_job_info(jdata, &info);
                    if (NULL == bkpt) {
                        /* no particular place was named, so any prior
                         * request for one no longer applies */
                        prte_remove_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT);
                    } else {
                        prte_set_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT,
                                           PRTE_ATTR_GLOBAL, bkpt, PMIX_STRING);
                        /* the daemons turn this into an envar for the app to
                         * read, but cache it as well so anything holding a
                         * PMIx handle on the job - a debugger tool, or the
                         * application itself - can simply ask for it */
                        PMIX_INFO_LOAD(&info, PMIX_BREAKPOINT, bkpt, PMIX_STRING);
                        pmix_server_cache_job_info(jdata, &info);
                        PMIX_INFO_DESTRUCT(&info);
                    }
                } else {
                    prte_remove_attribute(&jdata->attributes, PRTE_JOB_STOP_IN_APP);
                    prte_remove_attribute(&jdata->attributes, PRTE_JOB_BREAKPOINT);
                }

            } else if (PRTE_RTOS_TIMEOUT == tag) {
                /* 'tm', not 'n' - assigning the converted time to the loop
                 * index made the walk resume at "60" for "timeout=60", running
                 * off the end of the option array */
                tm = PMIX_CONVERT_TIME(ptr);
                prte_set_attribute(&jdata->attributes, PRTE_JOB_TIMEOUT, PRTE_ATTR_GLOBAL,
                                   &tm, PMIX_INT);

            } else if (PRTE_RTOS_SPAWN_TIMEOUT == tag) {
                tm = PMIX_CONVERT_TIME(ptr);
                prte_set_attribute(&jdata->attributes, PRTE_SPAWN_TIMEOUT, PRTE_ATTR_GLOBAL,
                                   &tm, PMIX_INT);

            } else if (PRTE_RTOS_STACK_TRACES == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_STACKTRACES, flag);

            } else if (PRTE_RTOS_REPORT_STATE == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_REPORT_STATE, flag);

            } else if (PRTE_RTOS_REPORT_CHILD_SEP == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_REPORT_CHILD_SEP, flag);
                /* This governs how the DVM reports its OVERALL exit status,
                 * not just this job's, and it is consumed as each job -
                 * including a child this job spawns - reaches teardown.  So
                 * record it on the daemon job too, where every teardown can
                 * see it; that is what donotlaunch/donotspawn above do for
                 * the same reason.  The !prte_persistent gate matches where
                 * it is read: only a one-shot DVM reports an exit status, so
                 * a job spawned into a persistent DVM cannot use this to
                 * change policy for everybody else. */
                if (!prte_persistent) {
                    djob = prte_get_job_data_object(PRTE_PROC_MY_NAME->nspace);
                    if (NULL != djob) {
                        set_bool_option(djob, PRTE_JOB_REPORT_CHILD_SEP, flag);
                    }
                }

            } else if (PRTE_RTOS_AGG_HELP == tag) {
                /* the attribute is the NEGATIVE of the directive: the user asks
                 * to aggregate help, PRTE_JOB_NOAGG_HELP records the request NOT
                 * to.  Driving it from the directive's own sense (as this did)
                 * made "aggregate-help=true" mean "do not aggregate". */
                flag = PMIX_CHECK_TRUE(&value);
                set_bool_option(jdata, PRTE_JOB_NOAGG_HELP, !flag);

            } else if (PRTE_RTOS_OUTPUT_PROCTABLE == tag) {
                if (NULL == ptr) {
                    /* no value provided, so assume stdout */
                    ptr = "-";
                }
                prte_set_attribute(&jdata->attributes, PRTE_JOB_OUTPUT_PROCTABLE,
                                    PRTE_ATTR_GLOBAL, ptr, PMIX_STRING);

            } else if (PRTE_RTOS_FWD_ENVIRON == tag) {
                flag = PMIX_CHECK_TRUE(&value);
                prte_set_bool_attribute(&jdata->attributes, PRTE_JOB_FWD_ENVIRONMENT, PRTE_ATTR_GLOBAL, flag);
            }
            /* PMIX_VALUE_LOAD copied the value string - release it before
             * the next directive overwrites the struct */
            PMIX_VALUE_DESTRUCT(&value);
        }
        PMIx_Argv_free(options);
    }
    /* if notify-error is set but neither recovery nor continuous were specified,
     * then notifications will not be given as we will terminate the job upon
     * error. Detect that situation, provide a show-help explaining the problem,
     * and then error out */
    if (PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_NOTIFY_ERRORS) &&
        !PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_RECOVERABLE) &&
        !PRTE_ATTR_IS_TRUE(&jdata->attributes, PRTE_JOB_CONTINUOUS)) {
        prte_show_help(PRTE_JOB_NSPACE(jdata), "help-state-base.txt", "bad-combination", true);
        return PRTE_ERR_SILENT;
    }
    return PRTE_SUCCESS;
}
