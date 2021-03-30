/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "constants.h"

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#    include <sys/wait.h>
#endif
#include <signal.h>
#ifdef HAVE_UTIL_H
#    include <util.h>
#endif
#ifdef HAVE_PTY_H
#    include <pty.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_TERMIOS_H
#    include <termios.h>
#    ifdef HAVE_TERMIO_H
#        include <termio.h>
#    endif
#endif
#ifdef HAVE_LIBUTIL_H
#    include <libutil.h>
#endif

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/name_fns.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"
#include "src/util/prte_pty.h"
#include "src/util/show_help.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/iof/iof.h"

int prte_iof_base_setup_prefork(prte_iof_base_io_conf_t *opts)
{
    int ret = -1;

    fflush(stdout);

    /* first check to make sure we can do ptys */
#if PRTE_ENABLE_PTY_SUPPORT
    if (opts->usepty) {
        /**
         * It has been reported that on MAC OS X 10.4 and prior one cannot
         * safely close the writing side of a pty before completly reading
         * all data inside.
         * There seems to be two issues: first all pending data is
         * discarded, and second it randomly generate kernel panics.
         * Apparently this issue was fixed in 10.5 so by now we use the
         * pty exactly as we use the pipes.
         * This comment is here as a reminder.
         */
        ret = prte_openpty(&(opts->p_stdout[0]), &(opts->p_stdout[1]), (char *) NULL,
                           (struct termios *) NULL, (struct winsize *) NULL);
    }
#else
    opts->usepty = 0;
#endif

    if (ret < 0) {
        opts->usepty = 0;
        if (pipe(opts->p_stdout) < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
            return PRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    if (opts->connect_stdin) {
        if (pipe(opts->p_stdin) < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
            return PRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    if (!prte_iof_base.redirect_app_stderr_to_stdout) {
        if (pipe(opts->p_stderr) < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
            return PRTE_ERR_SYS_LIMITS_PIPES;
        }
    }
    return PRTE_SUCCESS;
}

int prte_iof_base_setup_child(prte_iof_base_io_conf_t *opts, char ***env)
{
    int ret;

    if (opts->connect_stdin) {
        close(opts->p_stdin[1]);
    }
    close(opts->p_stdout[0]);
    if (!prte_iof_base.redirect_app_stderr_to_stdout) {
        close(opts->p_stderr[0]);
    }

    if (opts->usepty) {
        /* disable echo */
        struct termios term_attrs;
        if (tcgetattr(opts->p_stdout[1], &term_attrs) < 0) {
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        term_attrs.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | ECHONL);
        term_attrs.c_iflag &= ~(ICRNL | INLCR | ISTRIP | INPCK | IXON);
        term_attrs.c_oflag &= ~(
#ifdef OCRNL
            /* OS X 10.3 does not have this
               value defined */
            OCRNL |
#endif
            ONLCR);
        if (tcsetattr(opts->p_stdout[1], TCSANOW, &term_attrs) == -1) {
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        ret = dup2(opts->p_stdout[1], fileno(stdout));
        if (ret < 0) {
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        if (prte_iof_base.redirect_app_stderr_to_stdout) {
            ret = dup2(opts->p_stdout[1], fileno(stderr));
            if (ret < 0) {
                return PRTE_ERR_PIPE_SETUP_FAILURE;
            }
        }
        close(opts->p_stdout[1]);
    } else {
        if (opts->p_stdout[1] != fileno(stdout)) {
            ret = dup2(opts->p_stdout[1], fileno(stdout));
            if (ret < 0) {
                return PRTE_ERR_PIPE_SETUP_FAILURE;
            }
            if (prte_iof_base.redirect_app_stderr_to_stdout) {
                ret = dup2(opts->p_stdout[1], fileno(stderr));
                if (ret < 0) {
                    return PRTE_ERR_PIPE_SETUP_FAILURE;
                }
            }
            close(opts->p_stdout[1]);
        }
    }
    if (opts->connect_stdin) {
        if (opts->p_stdin[0] != fileno(stdin)) {
            ret = dup2(opts->p_stdin[0], fileno(stdin));
            if (ret < 0) {
                return PRTE_ERR_PIPE_SETUP_FAILURE;
            }
            close(opts->p_stdin[0]);
        }
    } else {
        int fd;

        /* connect input to /dev/null */
        fd = open("/dev/null", O_RDONLY, 0);
        if (fd != fileno(stdin)) {
            dup2(fd, fileno(stdin));
            close(fd);
        }
    }

    if (opts->p_stderr[1] != fileno(stderr)) {
        if (!prte_iof_base.redirect_app_stderr_to_stdout) {
            ret = dup2(opts->p_stderr[1], fileno(stderr));
            if (ret < 0)
                return PRTE_ERR_PIPE_SETUP_FAILURE;
            close(opts->p_stderr[1]);
        }
    }

    return PRTE_SUCCESS;
}

int prte_iof_base_setup_parent(const pmix_proc_t *name, prte_iof_base_io_conf_t *opts)
{
    int ret;

    /* connect stdin endpoint */
    if (opts->connect_stdin) {
        /* and connect the pty to stdin */
        ret = prte_iof.pull(name, PRTE_IOF_STDIN, opts->p_stdin[1]);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    /* connect read ends to IOF */
    ret = prte_iof.push(name, PRTE_IOF_STDOUT, opts->p_stdout[0]);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    if (!prte_iof_base.redirect_app_stderr_to_stdout) {
        ret = prte_iof.push(name, PRTE_IOF_STDERR, opts->p_stderr[0]);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

int prte_iof_base_setup_output_files(const pmix_proc_t *dst_name, prte_job_t *jobdat,
                                     prte_iof_proc_t *proct)
{
    int rc;
    char *dirname, *outdir, *outfile;
    int np, numdigs, fdout, i;
    char *p, **s;
    bool usejobid = true;

    /* see if we are to output to a directory */
    dirname = NULL;
    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_OUTPUT_TO_DIRECTORY, (void **) &dirname,
                           PMIX_STRING)
        && NULL != dirname) {
        np = jobdat->num_procs / 10;
        /* determine the number of digits required for max vpid */
        numdigs = 1;
        while (np > 0) {
            numdigs++;
            np = np / 10;
        }
        /* check for a conditional in the directory name */
        if (NULL != (p = strchr(dirname, ':'))) {
            *p = '\0';
            ++p;
            /* could me more than one directive */
            s = prte_argv_split(p, ',');
            for (i = 0; NULL != s[i]; i++) {
                if (0 == strcasecmp(s[i], "nojobid")) {
                    usejobid = false;
                } else if (0 == strcasecmp(s[i], "nocopy")) {
                    proct->copy = false;
                } else {
                    prte_show_help("help-iof-base", "unrecognized-directive", true,
                                   "output-directory", s[i]);
                    prte_argv_free(s);
                    return PRTE_ERROR;
                }
            }
        }

        /* construct the directory where the output files will go */
        if (usejobid) {
            prte_asprintf(&outdir, "%s/%s/rank.%0*u", dirname,
                          PRTE_LOCAL_JOBID_PRINT(proct->name.nspace), numdigs, proct->name.rank);
        } else {
            prte_asprintf(&outdir, "%s/rank.%0*u", dirname, numdigs, proct->name.rank);
        }
        /* ensure the directory exists */
        if (PRTE_SUCCESS != (rc = prte_os_dirpath_create(outdir, S_IRWXU | S_IRGRP | S_IXGRP))) {
            PRTE_ERROR_LOG(rc);
            free(outdir);
            return rc;
        }
        if (NULL != proct->revstdout && NULL == proct->revstdout->sink) {
            /* setup the stdout sink */
            prte_asprintf(&outfile, "%s/stdout", outdir);
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PRTE_ERROR_LOG(PRTE_ERR_FILE_OPEN_FAILURE);
                return PRTE_ERR_FILE_OPEN_FAILURE;
            }
            /* define a sink to that file descriptor */
            PRTE_IOF_SINK_DEFINE(&proct->revstdout->sink, dst_name, fdout, PRTE_IOF_STDOUT,
                                 prte_iof_base_write_handler);
        }

        if (NULL != proct->revstderr && NULL == proct->revstderr->sink) {
            /* if they asked for stderr to be combined with stdout, then we
             * only create one file and tell the IOF to put both streams
             * into it. Otherwise, we create separate files for each stream */
            if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_MERGE_STDERR_STDOUT, NULL,
                                   PMIX_BOOL)) {
                /* just use the stdout sink */
                PRTE_RETAIN(proct->revstdout->sink);
                proct->revstdout->sink->tag = PRTE_IOF_STDMERGE; // show that it is merged
                proct->revstderr->sink = proct->revstdout->sink;
            } else {
                prte_asprintf(&outfile, "%s/stderr", outdir);
                fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
                free(outfile);
                if (fdout < 0) {
                    /* couldn't be opened */
                    PRTE_ERROR_LOG(PRTE_ERR_FILE_OPEN_FAILURE);
                    return PRTE_ERR_FILE_OPEN_FAILURE;
                }
                /* define a sink to that file descriptor */
                PRTE_IOF_SINK_DEFINE(&proct->revstderr->sink, dst_name, fdout, PRTE_IOF_STDERR,
                                     prte_iof_base_write_handler);
            }
        }
        return PRTE_SUCCESS;
    }

    /* see if we are to output to a file */
    dirname = NULL;
    if (prte_get_attribute(&jobdat->attributes, PRTE_JOB_OUTPUT_TO_FILE, (void **) &dirname,
                           PMIX_STRING)
        && NULL != dirname) {
        np = jobdat->num_procs / 10;
        /* determine the number of digits required for max vpid */
        numdigs = 1;
        while (np > 0) {
            numdigs++;
            np = np / 10;
        }
        /* check for a conditional in the directory name */
        if (NULL != (p = strchr(dirname, ':'))) {
            *p = '\0';
            ++p;
            /* could me more than one directive */
            s = prte_argv_split(p, ',');
            for (i = 0; NULL != s[i]; i++) {
                if (0 == strcasecmp(s[i], "nocopy")) {
                    proct->copy = false;
                } else {
                    prte_show_help("help-iof-base", "unrecognized-directive", true,
                                   "output-filename", s[i]);
                    prte_argv_free(s);
                    return PRTE_ERROR;
                }
            }
        }

        /* construct the directory where the output files will go */
        outdir = prte_dirname(dirname);

        /* ensure the directory exists */
        if (PRTE_SUCCESS != (rc = prte_os_dirpath_create(outdir, S_IRWXU | S_IRGRP | S_IXGRP))) {
            PRTE_ERROR_LOG(rc);
            free(outdir);
            return rc;
        }
        if (NULL != proct->revstdout && NULL == proct->revstdout->sink) {
            /* setup the stdout sink */
            prte_asprintf(&outfile, "%s.%s.%0*u", dirname,
                          PRTE_LOCAL_JOBID_PRINT(proct->name.nspace), numdigs, proct->name.rank);
            fdout = open(outfile, O_CREAT | O_RDWR | O_TRUNC, 0644);
            free(outfile);
            if (fdout < 0) {
                /* couldn't be opened */
                PRTE_ERROR_LOG(PRTE_ERR_FILE_OPEN_FAILURE);
                return PRTE_ERR_FILE_OPEN_FAILURE;
            }
            /* define a sink to that file descriptor */
            PRTE_IOF_SINK_DEFINE(&proct->revstdout->sink, dst_name, fdout, PRTE_IOF_STDOUTALL,
                                 prte_iof_base_write_handler);
        }

        if (NULL != proct->revstderr && NULL == proct->revstderr->sink) {
            /* we only create one file - all output goes there */
            PRTE_RETAIN(proct->revstdout->sink);
            proct->revstdout->sink->tag = PRTE_IOF_STDMERGE; // show that it is merged
            proct->revstderr->sink = proct->revstdout->sink;
        }
        return PRTE_SUCCESS;
    }

    return PRTE_SUCCESS;
}

void prte_iof_base_check_target(prte_iof_proc_t *proct)
{
    prte_iof_request_t *preq;
    prte_iof_sink_t *sink;

    if (!proct->copy) {
        return;
    }

    /* see if any tools are waiting for this proc */
    PRTE_LIST_FOREACH(preq, &prte_iof_base.requests, prte_iof_request_t)
    {
        if (PMIX_CHECK_PROCID(&preq->target, &proct->name)) {
            if (NULL == proct->subscribers) {
                proct->subscribers = PRTE_NEW(prte_list_t);
            }
            if (PRTE_IOF_STDOUT & preq->stream) {
                PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDOUT, NULL);
                PMIX_XFER_PROCID(&sink->daemon, &preq->requestor);
                sink->exclusive = (PRTE_IOF_EXCLUSIVE & preq->stream);
                prte_list_append(proct->subscribers, &sink->super);
            }
            if (!prte_iof_base.redirect_app_stderr_to_stdout && PRTE_IOF_STDERR & preq->stream) {
                PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDERR, NULL);
                PMIX_XFER_PROCID(&sink->daemon, &preq->requestor);
                sink->exclusive = (PRTE_IOF_EXCLUSIVE & preq->stream);
                prte_list_append(proct->subscribers, &sink->super);
            }
            if (PRTE_IOF_STDDIAG & preq->stream) {
                PRTE_IOF_SINK_DEFINE(&sink, &proct->name, -1, PRTE_IOF_STDDIAG, NULL);
                PMIX_XFER_PROCID(&sink->daemon, &preq->requestor);
                sink->exclusive = (PRTE_IOF_EXCLUSIVE & preq->stream);
                prte_list_append(proct->subscribers, &sink->super);
            }
        }
    }
}
