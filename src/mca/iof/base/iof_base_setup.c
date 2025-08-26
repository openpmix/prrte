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
 * Copyright (c) 2017-2021 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
#endif

#include "src/mca/errmgr/errmgr.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_pty.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_tty.h"

#include "src/mca/iof/base/base.h"
#include "src/mca/iof/base/iof_base_setup.h"
#include "src/mca/iof/iof.h"

#define PTYNAME_MAXLEN  2048

int prte_iof_base_setup_prefork(prte_iof_base_io_conf_t *opts,
                                prte_job_t *jdata)
{
    int rc;
    struct termios interms, outterms, errterms, zterm, *terms;
    struct winsize inws, outws, errws, zws, *ws;
    size_t sz, offset;
    pmix_byte_object_t *bptr;

    fflush(stdout);

    memset(&zterm, 0, sizeof(struct termios));
    memset(&interms, 0, sizeof(struct termios));
    memset(&outterms, 0, sizeof(struct termios));
    memset(&errterms, 0, sizeof(struct termios));

    memset(&zws, 0, sizeof(struct winsize));
    memset(&inws, 0, sizeof(struct winsize));
    memset(&outws, 0, sizeof(struct winsize));
    memset(&errws, 0, sizeof(struct winsize));

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PTY_TERMIO, (void**)&bptr, PMIX_BYTE_OBJECT)) {
        // these are stored in stdin/stdout/stderr order
        offset = 0;
        sz = sizeof(struct termios);
        memcpy(&interms, bptr->bytes, sz);
        offset += sz;
        if (offset < bptr->size) {
            memcpy(&outterms, bptr->bytes + offset, sz);
            offset += sz;
            if (offset < bptr->size) {
                memcpy(&errterms, bptr->bytes + offset, sz);
            }
        }
        PMIx_Byte_object_free(bptr, 1);
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PTY_WSIZE, (void**)&bptr, PMIX_BYTE_OBJECT)) {
        // these are stored in stdin/stdout/stderr order
        offset = 0;
        sz = sizeof(struct winsize);
        memcpy(&inws, bptr->bytes, sz);
        offset += sz;
        if (offset < bptr->size) {
            memcpy(&outws, bptr->bytes + offset, sz);
            offset += sz;
            if (offset < bptr->size) {
                memcpy(&errws, bptr->bytes + offset, sz);
            }
        }
        PMIx_Byte_object_free(bptr, 1);
    }

    if (opts->usepty) {

        /* first check to make sure we can do ptys */
#if PRTE_ENABLE_PTY_SUPPORT == 0
        // we cannot execute this request as we weren't configured
        // with pty support
        PRTE_ERROR_LOG(PRTE_ERR_NOT_AVAILABLE);
        return PRTE_ERR_NOT_AVAILABLE;
#endif

        if (opts->connect_stdin) {
            if (memcmp(&zterm, &interms, sizeof(struct termios))) {
                terms = NULL;
            } else {
                terms = &interms;
            }
            if (memcmp(&zws, &inws, sizeof(struct winsize))) {
                ws = NULL;
            } else {
                ws = &inws;
            }
            rc = pmix_openpty(&opts->p_stdin[0], &opts->p_stdin[1],
                              NULL, terms, ws);
            if (0 != rc) {
                    PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
                    return PRTE_ERR_PIPE_SETUP_FAILURE;
            }
        }

        if (memcmp(&zterm, &outterms, sizeof(struct termios))) {
            terms = NULL;
        } else {
            terms = &outterms;
        }
        if (memcmp(&zws, &outws, sizeof(struct winsize))) {
            ws = NULL;
        } else {
            ws = &outws;
        }
        rc = pmix_openpty(&opts->p_stdout[0], &opts->p_stdout[1],
                          NULL, terms, ws);
        if (0 != rc) {
                PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
                return PRTE_ERR_PIPE_SETUP_FAILURE;
        }

        if (memcmp(&zterm, &errterms, sizeof(struct termios))) {
            terms = NULL;
        } else {
            terms = &errterms;
        }
        if (memcmp(&zws, &errws, sizeof(struct winsize))) {
            ws = NULL;
        } else {
            ws = &errws;
        }
        rc = pmix_openpty(&opts->p_stderr[0], &opts->p_stderr[1],
                          NULL, terms, ws);
        if (0 != rc) {
                PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
                return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        return PRTE_SUCCESS;
    }  // if usepty


    // using pipes

    if (opts->connect_stdin) {
        if (pipe(opts->p_stdin) < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
            return PRTE_ERR_SYS_LIMITS_PIPES;
        }
    }

    if (pipe(opts->p_stdout) < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
        return PRTE_ERR_SYS_LIMITS_PIPES;
    }

    if (pipe(opts->p_stderr) < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_SYS_LIMITS_PIPES);
        return PRTE_ERR_SYS_LIMITS_PIPES;
    }

    return PRTE_SUCCESS;
}

int prte_iof_base_setup_child(prte_iof_base_io_conf_t *opts,
                              char ***env)
{
    int ret;
    int fd;
    PRTE_HIDE_UNUSED_PARAMS(env);

    if (opts->connect_stdin) {
        close(opts->p_stdin[0]);
    }
    close(opts->p_stdout[0]);
    close(opts->p_stderr[0]);

    if (opts->usepty) {

        // we are the slave end
        if (opts->connect_stdin) {
            ret = pmix_fd_dup2(opts->p_stdin[1], 0);
            if (ret < 0) {
                PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
                return PRTE_ERR_PIPE_SETUP_FAILURE;
            }
            if (ret != opts->p_stdin[1]) {
                close(opts->p_stdin[1]);
            }
        } else {

            /* connect input to /dev/null */
            fd = open("/dev/null", O_RDONLY, 0);
            ret = pmix_fd_dup2(fd, 0);
            if (ret != 0) {
                close(fd);
            }
        }

#if 0
        /* disable echo */
        struct termios term_attrs;
        if (tcgetattr(opts->p_stdout[1], &term_attrs) < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
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
            PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
#endif

        ret = pmix_fd_dup2(opts->p_stdout[1], 1);
        if (ret < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        if (ret != opts->p_stdout[1]) {
            close(opts->p_stdout[1]);
        }
        ret = pmix_fd_dup2(opts->p_stderr[1], 2);
        if (ret < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        if (ret != opts->p_stderr[1]) {
            close(opts->p_stderr[1]);
        }

        return PRTE_SUCCESS;
    }


    // we are using pipes
    if (opts->connect_stdin) {
        close(opts->p_stdin[1]);
    }
    close(opts->p_stdout[0]);
    close(opts->p_stderr[0]);

    if (opts->connect_stdin) {
        ret = pmix_fd_dup2(opts->p_stdin[0], 0);
        if (ret < 0) {
            PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
            return PRTE_ERR_PIPE_SETUP_FAILURE;
        }
        if (ret != opts->p_stdin[0]) {
            close(opts->p_stdin[0]);
        }

    } else {
        /* connect input to /dev/null */
        fd = open("/dev/null", O_RDONLY, 0);
        if (fd != fileno(stdin)) {
            dup2(fd, fileno(stdin));
        }
        close(fd);
    }

    ret = pmix_fd_dup2(opts->p_stdout[1], 1);
    if (ret < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
        return PRTE_ERR_PIPE_SETUP_FAILURE;
    }
    if (ret != opts->p_stdout[1]) {
        close(opts->p_stdout[1]);
    }

    ret = pmix_fd_dup2(opts->p_stderr[1], 2);
    if (ret < 0) {
        PRTE_ERROR_LOG(PRTE_ERR_PIPE_SETUP_FAILURE);
        return PRTE_ERR_PIPE_SETUP_FAILURE;
    }
    if (ret != opts->p_stderr[1]) {
        close(opts->p_stderr[1]);
    }

    return PRTE_SUCCESS;
}

int prte_iof_base_setup_parent(const pmix_proc_t *name,
                               prte_iof_base_io_conf_t *opts)
{
    int ret;

    if (opts->usepty) {
        if (opts->connect_stdin) {
            ret = prte_iof.pull(name, PRTE_IOF_STDIN, opts->p_stdin[0]);
            if (PRTE_SUCCESS != ret) {
                PRTE_ERROR_LOG(ret);
                return ret;
            }
        }

        ret = prte_iof.push(name, PRTE_IOF_STDOUT, opts->p_stdout[0]);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }

        ret = prte_iof.push(name, PRTE_IOF_STDERR, opts->p_stderr[0]);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }

        return PRTE_SUCCESS;
    }

    // using pipes

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

    ret = prte_iof.push(name, PRTE_IOF_STDERR, opts->p_stderr[0]);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }

    return PRTE_SUCCESS;
}
