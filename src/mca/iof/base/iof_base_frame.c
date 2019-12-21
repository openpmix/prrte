/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <stdio.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "src/util/basename.h"

#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/mca/rml/rml.h"

#include "src/mca/iof/iof.h"
#include "src/mca/iof/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_base_component_t struct.
 */

#include "src/mca/iof/base/static-components.h"

prrte_iof_base_module_t prrte_iof = {0};


/*
 * Global variables
 */

prrte_iof_base_t prrte_iof_base = {0};

static int prrte_iof_base_register(prrte_mca_base_register_flag_t flags)
{
    /* check for maximum number of pending output messages */
    prrte_iof_base.output_limit = (size_t) INT_MAX;
    (void) prrte_mca_base_var_register("prrte", "iof", "base", "output_limit",
                                       "Maximum backlog of output messages [default: unlimited]",
                                       PRRTE_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_iof_base.output_limit);

    /* Redirect application stderr to stdout (at source) */
    prrte_iof_base.redirect_app_stderr_to_stdout = false;
    (void) prrte_mca_base_var_register("prrte", "iof","base", "redirect_app_stderr_to_stdout",
                                       "Redirect application stderr to stdout at source (default: false)",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_iof_base.redirect_app_stderr_to_stdout);

    return PRRTE_SUCCESS;
}

static int prrte_iof_base_close(void)
{
    /* Close the selected component */
    if (NULL != prrte_iof.finalize) {
        prrte_iof.finalize();
    }

    if (!PRRTE_PROC_IS_DAEMON) {
        if (NULL != prrte_iof_base.iof_write_stdout) {
            PRRTE_RELEASE(prrte_iof_base.iof_write_stdout);
        }
        if (!prrte_xml_output && NULL != prrte_iof_base.iof_write_stderr) {
            PRRTE_RELEASE(prrte_iof_base.iof_write_stderr);
        }
    }
    return prrte_mca_base_framework_components_close(&prrte_iof_base_framework, NULL);
}


/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_iof_base_open(prrte_mca_base_open_flag_t flags)
{
    int xmlfd;

    /* daemons do not need to do this as they do not write out stdout/err */
    if (!PRRTE_PROC_IS_DAEMON) {
        if (prrte_xml_output) {
            if (NULL != prrte_xml_fp) {
                /* user wants all xml-formatted output sent to file */
                xmlfd = fileno(prrte_xml_fp);
            } else {
                xmlfd = 1;
            }
            /* setup the stdout event */
            PRRTE_IOF_SINK_DEFINE(&prrte_iof_base.iof_write_stdout, PRRTE_PROC_MY_NAME,
                                 xmlfd, PRRTE_IOF_STDOUT, prrte_iof_base_write_handler);
            /* don't create a stderr event - all output will go to
             * the stdout channel
             */
        } else {
            /* setup the stdout event */
            PRRTE_IOF_SINK_DEFINE(&prrte_iof_base.iof_write_stdout, PRRTE_PROC_MY_NAME,
                                 1, PRRTE_IOF_STDOUT, prrte_iof_base_write_handler);
            /* setup the stderr event */
            PRRTE_IOF_SINK_DEFINE(&prrte_iof_base.iof_write_stderr, PRRTE_PROC_MY_NAME,
                                 2, PRRTE_IOF_STDERR, prrte_iof_base_write_handler);
        }

        /* do NOT set these file descriptors to non-blocking. If we do so,
         * we set the file descriptor to non-blocking for everyone that has
         * that file descriptor, which includes everyone else in our shell
         * pipeline chain.  (See
         * http://lists.freebsd.org/pipermail/freebsd-hackers/2005-January/009742.html).
         * This causes things like "mpirun -np 1 big_app | cat" to lose
         * output, because cat's stdout is then ALSO non-blocking and cat
         * isn't built to deal with that case (same with almost all other
         * unix text utils).
         */
    }

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_iof_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, iof, "PRRTE I/O Forwarding",
                                 prrte_iof_base_register, prrte_iof_base_open, prrte_iof_base_close,
                                 prrte_iof_base_static_components, 0);


/* class instances */
static void prrte_iof_job_construct(prrte_iof_job_t *ptr)
{
    ptr->jdata = NULL;
    PRRTE_CONSTRUCT(&ptr->xoff, prrte_bitmap_t);
}
static void prrte_iof_job_destruct(prrte_iof_job_t *ptr)
{
    if (NULL != ptr->jdata) {
        PRRTE_RELEASE(ptr->jdata);
    }
    PRRTE_DESTRUCT(&ptr->xoff);
}
PRRTE_CLASS_INSTANCE(prrte_iof_job_t,
                   prrte_object_t,
                   prrte_iof_job_construct,
                   prrte_iof_job_destruct);

static void prrte_iof_base_proc_construct(prrte_iof_proc_t* ptr)
{
    ptr->stdinev = NULL;
    ptr->revstdout = NULL;
    ptr->revstderr = NULL;
    ptr->subscribers = NULL;
    ptr->copy = true;
}
static void prrte_iof_base_proc_destruct(prrte_iof_proc_t* ptr)
{
    if (NULL != ptr->stdinev) {
        PRRTE_RELEASE(ptr->stdinev);
    }
    if (NULL != ptr->revstdout) {
        PRRTE_RELEASE(ptr->revstdout);
    }
    if (NULL != ptr->revstderr) {
        PRRTE_RELEASE(ptr->revstderr);
    }
    if (NULL != ptr->subscribers) {
        PRRTE_LIST_RELEASE(ptr->subscribers);
    }
}
PRRTE_CLASS_INSTANCE(prrte_iof_proc_t,
                   prrte_list_item_t,
                   prrte_iof_base_proc_construct,
                   prrte_iof_base_proc_destruct);


static void prrte_iof_base_sink_construct(prrte_iof_sink_t* ptr)
{
    ptr->daemon.jobid = PRRTE_JOBID_INVALID;
    ptr->daemon.vpid = PRRTE_VPID_INVALID;
    ptr->wev = PRRTE_NEW(prrte_iof_write_event_t);
    ptr->xoff = false;
    ptr->exclusive = false;
    ptr->closed = false;
}
static void prrte_iof_base_sink_destruct(prrte_iof_sink_t* ptr)
{
    if (NULL != ptr->wev) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                             "%s iof: closing sink for process %s on fd %d",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                             PRRTE_NAME_PRINT(&ptr->name), ptr->wev->fd));
        PRRTE_RELEASE(ptr->wev);
    }
}
PRRTE_CLASS_INSTANCE(prrte_iof_sink_t,
                   prrte_list_item_t,
                   prrte_iof_base_sink_construct,
                   prrte_iof_base_sink_destruct);


static void prrte_iof_base_read_event_construct(prrte_iof_read_event_t* rev)
{
    rev->proc = NULL;
    rev->fd = -1;
    rev->active = false;
    rev->ev = prrte_event_alloc();
    rev->sink = NULL;
    rev->tv.tv_sec = 0;
    rev->tv.tv_usec = 0;
}
static void prrte_iof_base_read_event_destruct(prrte_iof_read_event_t* rev)
{
    prrte_iof_proc_t *proct = (prrte_iof_proc_t*)rev->proc;

    if (0 <= rev->fd) {
        prrte_event_free(rev->ev);
        PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                             "%s iof: closing fd %d for process %s",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), rev->fd,
                             (NULL == proct) ? "UNKNOWN" : PRRTE_NAME_PRINT(&proct->name)));
        close(rev->fd);
        rev->fd = -1;
    } else {
        free(rev->ev);
    }
    if (NULL != rev->sink) {
        PRRTE_RELEASE(rev->sink);
    }
    if (NULL != proct) {
        PRRTE_RELEASE(proct);
    }
}
PRRTE_CLASS_INSTANCE(prrte_iof_read_event_t,
                   prrte_object_t,
                   prrte_iof_base_read_event_construct,
                   prrte_iof_base_read_event_destruct);

static void prrte_iof_base_write_event_construct(prrte_iof_write_event_t* wev)
{
    wev->pending = false;
    wev->always_writable = false;
    wev->fd = -1;
    PRRTE_CONSTRUCT(&wev->outputs, prrte_list_t);
    wev->ev = prrte_event_alloc();
    wev->tv.tv_sec = 0;
    wev->tv.tv_usec = 0;
}
static void prrte_iof_base_write_event_destruct(prrte_iof_write_event_t* wev)
{
    if (0 <= wev->fd) {
        prrte_event_free(wev->ev);
    } else {
        free(wev->ev);
    }
    if (PRRTE_PROC_IS_MASTER && NULL != prrte_xml_fp) {
        int xmlfd = fileno(prrte_xml_fp);
        if (xmlfd == wev->fd) {
            /* don't close this one - will get it later */
            PRRTE_DESTRUCT(&wev->outputs);
            return;
        }
    }
    if (2 < wev->fd) {
        PRRTE_OUTPUT_VERBOSE((20, prrte_iof_base_framework.framework_output,
                             "%s iof: closing fd %d for write event",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), wev->fd));
        close(wev->fd);
    }
    PRRTE_DESTRUCT(&wev->outputs);
}
PRRTE_CLASS_INSTANCE(prrte_iof_write_event_t,
                   prrte_list_item_t,
                   prrte_iof_base_write_event_construct,
                   prrte_iof_base_write_event_destruct);

PRRTE_CLASS_INSTANCE(prrte_iof_write_output_t,
                   prrte_list_item_t,
                   NULL, NULL);
