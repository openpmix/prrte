/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      FUJITSU LIMITED.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Geoffroy Vallee. All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#else
#    ifdef HAVE_SYS_FCNTL_H
#        include <sys/fcntl.h>
#    endif
#endif

#include <signal.h>
#include <string.h>

#include "constants.h"
#include "src/mca/prtebacktrace/prtebacktrace.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/error.h"
#include "src/util/pmix_output.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"
#include "src/util/stacktrace.h"

#ifndef _NSIG
#    define _NSIG 32
#endif

#define HOSTFORMAT "[%s:%05d] "

int prte_stacktrace_output_fileno = -1;
static char *prte_stacktrace_output_filename_base = NULL;
static size_t prte_stacktrace_output_filename_max_len = 0;
static char *unable_to_print_msg = "Unable to print stack trace!\n";

/*
 * Set the stacktrace filename:
 * stacktrace.PID
 * -or, if VPID is available-
 * stacktrace.VPID.PID
 */
static void set_stacktrace_filename(void)
{
    snprintf(prte_stacktrace_output_filename, prte_stacktrace_output_filename_max_len, "%s.%lu.%lu",
             prte_stacktrace_output_filename_base, (unsigned long) PRTE_PROC_MY_NAME->rank,
             (unsigned long) getpid());

    return;
}

/**
 * This function is being called as a signal-handler in response
 * to a user-specified signal (e.g. SIGFPE or SIGSEGV).
 * For Linux/Glibc, it then uses backtrace and backtrace_symbols_fd
 * to figure the current stack and print that out to stderr.
 * Where available, the BSD libexecinfo is used to provide Linux/Glibc
 * compatible backtrace and backtrace_symbols_fd functions.
 *
 *  @param signo with the signal number raised
 *  @param info with information regarding the reason/send of the signal
 *  @param p
 *
 * FIXME: Should distinguish for systems, which don't have siginfo...
 */
#if PRTE_WANT_PRETTY_PRINT_STACKTRACE
static void show_stackframe(int signo, siginfo_t *info, void *p)
{
    char print_buffer[1024];
    char *tmp = print_buffer;
    int size = sizeof(print_buffer);
    int ret;
    char *si_code_str = "";
    PRTE_HIDE_UNUSED_PARAMS(p);

    /* Do not print the stack trace */
    if (0 > prte_stacktrace_output_fileno && 0 == prte_stacktrace_output_filename_max_len) {
        /* Raise the signal again, so we don't accidentally mask critical signals.
         * For critical signals, it is preferred that we call 'raise' instead of
         * 'exit' or 'abort' so that the return status is set properly for this
         * process.
         */
        signal(signo, SIG_DFL);
        raise(signo);

        return;
    }

    /* Update the file name with the RANK, if available */
    if (0 < prte_stacktrace_output_filename_max_len) {
        set_stacktrace_filename();
        prte_stacktrace_output_fileno = open(prte_stacktrace_output_filename,
                                             O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        if (0 > prte_stacktrace_output_fileno) {
            pmix_output(0,
                        "Error: Failed to open the stacktrace output file. Default: "
                        "stderr\n\tFilename: %s\n\tErrno: %s",
                        prte_stacktrace_output_filename, strerror(errno));
            prte_stacktrace_output_fileno = fileno(stderr);
        }
    }

    /* write out the footer information */
    memset(print_buffer, 0, sizeof(print_buffer));
    ret = snprintf(print_buffer, sizeof(print_buffer),
                   HOSTFORMAT "*** Process received signal ***\n", prte_process_info.nodename,
                   getpid());
    if (-1 == write(prte_stacktrace_output_fileno, print_buffer, ret)) {
        return;
    }

    memset(print_buffer, 0, sizeof(print_buffer));

#    ifdef HAVE_STRSIGNAL
    ret = snprintf(tmp, size, HOSTFORMAT "Signal: %s (%d)\n", prte_process_info.nodename, getpid(),
                   strsignal(signo), signo);
#    else
    ret = snprintf(tmp, size, HOSTFORMAT "Signal: %d\n", prte_process_info.nodename, getpid(),
                   signo);
#    endif
    size -= ret;
    tmp += ret;

    if (NULL != info) {
        switch (signo) {
        case SIGILL:
            switch (info->si_code) {
#    ifdef ILL_ILLOPC
            case ILL_ILLOPC:
                si_code_str = "Illegal opcode";
                break;
#    endif
#    ifdef ILL_ILLOPN
            case ILL_ILLOPN:
                si_code_str = "Illegal operand";
                break;
#    endif
#    ifdef ILL_ILLADR
            case ILL_ILLADR:
                si_code_str = "Illegal addressing mode";
                break;
#    endif
#    ifdef ILL_ILLTRP
            case ILL_ILLTRP:
                si_code_str = "Illegal trap";
                break;
#    endif
#    ifdef ILL_PRVOPC
            case ILL_PRVOPC:
                si_code_str = "Privileged opcode";
                break;
#    endif
#    ifdef ILL_PRVREG
            case ILL_PRVREG:
                si_code_str = "Privileged register";
                break;
#    endif
#    ifdef ILL_COPROC
            case ILL_COPROC:
                si_code_str = "Coprocessor error";
                break;
#    endif
#    ifdef ILL_BADSTK
            case ILL_BADSTK:
                si_code_str = "Internal stack error";
                break;
#    endif
            }
            break;
        case SIGFPE:
            switch (info->si_code) {
#    ifdef FPE_INTDIV
            case FPE_INTDIV:
                si_code_str = "Integer divide-by-zero";
                break;
#    endif
#    ifdef FPE_INTOVF
            case FPE_INTOVF:
                si_code_str = "Integer overflow";
                break;
#    endif
            case FPE_FLTDIV:
                si_code_str = "Floating point divide-by-zero";
                break;
            case FPE_FLTOVF:
                si_code_str = "Floating point overflow";
                break;
            case FPE_FLTUND:
                si_code_str = "Floating point underflow";
                break;
#    ifdef FPE_FLTRES
            case FPE_FLTRES:
                si_code_str = "Floating point inexact result";
                break;
#    endif
#    ifdef FPE_FLTINV
            case FPE_FLTINV:
                si_code_str = "Invalid floating point operation";
                break;
#    endif
#    ifdef FPE_FLTSUB
            case FPE_FLTSUB:
                si_code_str = "Subscript out of range";
                break;
#    endif
            }
            break;
        case SIGSEGV:
            switch (info->si_code) {
#    ifdef SEGV_MAPERR
            case SEGV_MAPERR:
                si_code_str = "Address not mapped";
                break;
#    endif
#    ifdef SEGV_ACCERR
            case SEGV_ACCERR:
                si_code_str = "Invalid permissions";
                break;
#    endif
            }
            break;
        case SIGBUS:
            switch (info->si_code) {
#    ifdef BUS_ADRALN
            case BUS_ADRALN:
                si_code_str = "Invalid address alignment";
                break;
#    endif
#    ifdef BUS_ADRERR
            case BUS_ADRERR:
                si_code_str = "Non-existant physical address";
                break;
#    endif
#    ifdef BUS_OBJERR
            case BUS_OBJERR:
                si_code_str = "Object-specific hardware error";
                break;
#    endif
            }
            break;
        case SIGTRAP:
            switch (info->si_code) {
#    ifdef TRAP_BRKPT
            case TRAP_BRKPT:
                si_code_str = "Process breakpoint";
                break;
#    endif
#    ifdef TRAP_TRACE
            case TRAP_TRACE:
                si_code_str = "Process trace trap";
                break;
#    endif
            }
            break;
        case SIGCHLD:
            switch (info->si_code) {
#    ifdef CLD_EXITED
            case CLD_EXITED:
                si_code_str = "Child has exited";
                break;
#    endif
#    ifdef CLD_KILLED
            case CLD_KILLED:
                si_code_str = "Child has terminated abnormally and did not create a core file";
                break;
#    endif
#    ifdef CLD_DUMPED
            case CLD_DUMPED:
                si_code_str = "Child has terminated abnormally and created a core file";
                break;
#    endif
#    ifdef CLD_WTRAPPED
            case CLD_TRAPPED:
                si_code_str = "Traced child has trapped";
                break;
#    endif
#    ifdef CLD_STOPPED
            case CLD_STOPPED:
                si_code_str = "Child has stopped";
                break;
#    endif
#    ifdef CLD_CONTINUED
            case CLD_CONTINUED:
                si_code_str = "Stopped child has continued";
                break;
#    endif
            }
            break;
#    ifdef SIGPOLL
        case SIGPOLL:
            switch (info->si_code) {
#        ifdef POLL_IN
            case POLL_IN:
                si_code_str = "Data input available";
                break;
#        endif
#        ifdef POLL_OUT
            case POLL_OUT:
                si_code_str = "Output buffers available";
                break;
#        endif
#        ifdef POLL_MSG
            case POLL_MSG:
                si_code_str = "Input message available";
                break;
#        endif
#        ifdef POLL_ERR
            case POLL_ERR:
                si_code_str = "I/O error";
                break;
#        endif
#        ifdef POLL_PRI
            case POLL_PRI:
                si_code_str = "High priority input available";
                break;
#        endif
#        ifdef POLL_HUP
            case POLL_HUP:
                si_code_str = "Device disconnected";
                break;
#        endif
            }
            break;
#    endif /* SIGPOLL */
        default:
            switch (info->si_code) {
#    ifdef SI_ASYNCNL
            case SI_ASYNCNL:
                si_code_str = "SI_ASYNCNL";
                break;
#    endif
#    ifdef SI_SIGIO
            case SI_SIGIO:
                si_code_str = "Queued SIGIO";
                break;
#    endif
#    ifdef SI_ASYNCIO
            case SI_ASYNCIO:
                si_code_str = "Asynchronous I/O request completed";
                break;
#    endif
#    ifdef SI_MESGQ
            case SI_MESGQ:
                si_code_str = "Message queue state changed";
                break;
#    endif
            case SI_TIMER:
                si_code_str = "Timer expiration";
                break;
            case SI_QUEUE:
                si_code_str = "Sigqueue() signal";
                break;
            case SI_USER:
                si_code_str = "User function (kill, sigsend, abort, etc.)";
                break;
#    ifdef SI_KERNEL
            case SI_KERNEL:
                si_code_str = "Kernel signal";
                break;
#    endif
/* Dragonfly defines SI_USER and SI_UNDEFINED both as zero: */
/* For some reason, the PGI compiler will not let us combine these two
   #if tests into a single statement.  Sigh. */
#    if defined(SI_UNDEFINED)
#        if SI_UNDEFINED != SI_USER
            case SI_UNDEFINED:
                si_code_str = "Undefined code";
                break;
#        endif
#    endif
            }
        }

        /* print signal errno information */
        if (0 != info->si_errno) {
            ret = snprintf(tmp, size, HOSTFORMAT "Associated errno: %s (%d)\n",
                           prte_process_info.nodename, getpid(), strerror(info->si_errno),
                           info->si_errno);
            size -= ret;
            tmp += ret;
        }

        ret = snprintf(tmp, size, HOSTFORMAT "Signal code: %s (%d)\n", prte_process_info.nodename,
                       getpid(), si_code_str, info->si_code);
        size -= ret;
        tmp += ret;

        switch (signo) {
        case SIGILL:
        case SIGFPE:
        case SIGSEGV:
        case SIGBUS: {
            ret = snprintf(tmp, size, HOSTFORMAT "Failing at address: %p\n",
                           prte_process_info.nodename, getpid(), info->si_addr);
            size -= ret;
            tmp += ret;
            break;
        }
        case SIGCHLD: {
            ret = snprintf(tmp, size, HOSTFORMAT "Sending PID: %d, Sending UID: %d, Status: %d\n",
                           prte_process_info.nodename, getpid(), info->si_pid, info->si_uid,
                           info->si_status);
            size -= ret;
            tmp += ret;
            break;
        }
#    ifdef SIGPOLL
        case SIGPOLL: {
#        ifdef HAVE_SIGINFO_T_SI_FD
            ret = snprintf(tmp, size, HOSTFORMAT "Band event: %ld, File Descriptor : %d\n",
                           prte_process_info.nodename, getpid(), (long) info->si_band, info->si_fd);
#        elif HAVE_SIGINFO_T_SI_BAND
            ret = snprintf(tmp, size, HOSTFORMAT "Band event: %ld\n", prte_process_info.nodename,
                           getpid(), (long) info->si_band);
#        else
            ret = 0;
#        endif
            size -= ret;
            tmp += ret;
            break;
        }
#    endif
        }
    } else {
        ret = snprintf(tmp, size,
                       HOSTFORMAT "siginfo is NULL, additional information unavailable\n",
                       prte_process_info.nodename, getpid());
        size -= ret;
        tmp += ret;
    }

    /* write out the signal information generated above */
    if (-1 == write(prte_stacktrace_output_fileno, print_buffer, sizeof(print_buffer) - size)) {
        return;
    }

    /* print out the stack trace */
    snprintf(print_buffer, sizeof(print_buffer), HOSTFORMAT, prte_process_info.nodename, getpid());
    ret = prte_backtrace_print(NULL, print_buffer, 2);
    if (PRTE_SUCCESS != ret) {
        if (-1
            == write(prte_stacktrace_output_fileno, unable_to_print_msg,
                     strlen(unable_to_print_msg))) {
            return;
        }
    }

    /* write out the footer information */
    memset(print_buffer, 0, sizeof(print_buffer));
    ret = snprintf(print_buffer, sizeof(print_buffer), HOSTFORMAT "*** End of error message ***\n",
                   prte_process_info.nodename, getpid());
    if (ret > 0) {
        if (-1 == write(prte_stacktrace_output_fileno, print_buffer, ret)) {
            return;
        }
    } else {
        if (-1
            == write(prte_stacktrace_output_fileno, unable_to_print_msg,
                     strlen(unable_to_print_msg))) {
            return;
        }
    }

    if (fileno(stdout) != prte_stacktrace_output_fileno
        && fileno(stderr) != prte_stacktrace_output_fileno) {
        close(prte_stacktrace_output_fileno);
        prte_stacktrace_output_fileno = -1;
    }

    /* Raise the signal again, so we don't accidentally mask critical signals.
     * For critical signals, it is preferred that we call 'raise' instead of
     * 'exit' or 'abort' so that the return status is set properly for this
     * process.
     */
    signal(signo, SIG_DFL);
    raise(signo);
}

#endif /* PRTE_WANT_PRETTY_PRINT_STACKTRACE */

#if PRTE_WANT_PRETTY_PRINT_STACKTRACE
void prte_stackframe_output(int stream)
{
    int traces_size;
    char **traces;

    /* print out the stack trace */
    if (PRTE_SUCCESS == prte_backtrace_buffer(&traces, &traces_size)) {
        int i;
        /* since we have the opportunity, strip off the bottom two
           function calls, which will be this function and
           prte_backtrace_buffer(). */
        for (i = 2; i < traces_size; ++i) {
            pmix_output(stream, "%s", traces[i]);
        }
    } else {
        /* Do not print the stack trace */
        if (0 > prte_stacktrace_output_fileno && 0 == prte_stacktrace_output_filename_max_len) {
            return;
        }

        /* Update the file name with the RANK, if available */
        if (0 < prte_stacktrace_output_filename_max_len) {
            set_stacktrace_filename();
            prte_stacktrace_output_fileno = open(prte_stacktrace_output_filename,
                                                 O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
            if (0 > prte_stacktrace_output_fileno) {
                pmix_output(0,
                            "Error: Failed to open the stacktrace output file. Default: "
                            "stderr\n\tFilename: %s\n\tErrno: %s",
                            prte_stacktrace_output_filename, strerror(errno));
                prte_stacktrace_output_fileno = fileno(stderr);
            }
        }

        prte_backtrace_print(NULL, NULL, 2);

        if (fileno(stdout) != prte_stacktrace_output_fileno
            && fileno(stderr) != prte_stacktrace_output_fileno) {
            close(prte_stacktrace_output_fileno);
            prte_stacktrace_output_fileno = -1;
        }
    }
}

char *prte_stackframe_output_string(void)
{
    int traces_size, i;
    size_t len;
    char *output, **traces;

    len = 0;
    if (PRTE_SUCCESS != prte_backtrace_buffer(&traces, &traces_size)) {
        return NULL;
    }

    /* Calculate the space needed for the string */
    for (i = 3; i < traces_size; i++) {
        if (NULL == traces[i]) {
            break;
        }
        len += strlen(traces[i]) + 1;
    }

    output = (char *) malloc(len + 1);
    if (NULL == output) {
        return NULL;
    }

    *output = '\0';
    for (i = 3; i < traces_size; i++) {
        if (NULL == traces[i]) {
            break;
        }
        strcat(output, traces[i]);
        strcat(output, "\n");
    }

    free(traces);
    return output;
}

#endif /* PRTE_WANT_PRETTY_PRINT_STACKTRACE */

/**
 * Here we register the show_stackframe function for signals
 * passed to PRTE by the mpi_signal-parameter passed to mpirun
 * by the user.
 *
 *  @returnvalue PRTE_SUCCESS
 *  @returnvalue PRTE_ERR_BAD_PARAM if the value in the signal-list
 *    is not a valid signal-number
 *
 */
int prte_util_register_stackhandlers(void)
{
#if PRTE_WANT_PRETTY_PRINT_STACKTRACE
    struct sigaction act, old;
    char *tmp;
    char *next;
    bool complain, showed_help = false;

    /* Setup the output stream to use */
    if (NULL == prte_stacktrace_output_filename
        || 0 == strcasecmp(prte_stacktrace_output_filename, "none")) {
        prte_stacktrace_output_fileno = -1;
    } else if (0 == strcasecmp(prte_stacktrace_output_filename, "stdout")) {
        prte_stacktrace_output_fileno = fileno(stdout);
    } else if (0 == strcasecmp(prte_stacktrace_output_filename, "stderr")) {
        prte_stacktrace_output_fileno = fileno(stderr);
    } else if (0 == strcasecmp(prte_stacktrace_output_filename, "file")
               || 0 == strcasecmp(prte_stacktrace_output_filename, "file:")) {
        prte_stacktrace_output_filename_base = strdup("stacktrace");

        free(prte_stacktrace_output_filename);
        // Magic number: 8 = space for .PID and .RANK (allow 7 digits each)
        prte_stacktrace_output_filename_max_len = strlen("stacktrace") + 8 + 8;
        prte_stacktrace_output_filename = (char *) malloc(
            sizeof(char) * prte_stacktrace_output_filename_max_len);
        set_stacktrace_filename();
        prte_stacktrace_output_fileno = -1;
    } else if (0 == strncasecmp(prte_stacktrace_output_filename, "file:", 5)) {
        char *filename_cpy = NULL;
        next = strchr(prte_stacktrace_output_filename, ':');
        if (NULL == next) {
            free(prte_stacktrace_output_filename);
            free(filename_cpy);
            return PRTE_ERR_NOT_FOUND;
        }
        next++; // move past the ':' to the filename specified

        prte_stacktrace_output_filename_base = strdup(next);

        free(prte_stacktrace_output_filename);
        // Magic number: 8 = space for .PID and .RANK (allow 7 digits each)
        prte_stacktrace_output_filename_max_len = strlen(prte_stacktrace_output_filename_base) + 8
                                                  + 8;
        prte_stacktrace_output_filename = (char *) malloc(
            sizeof(char) * prte_stacktrace_output_filename_max_len);
        set_stacktrace_filename();
        prte_stacktrace_output_fileno = -1;

        free(filename_cpy);
    } else {
        prte_stacktrace_output_fileno = fileno(stderr);
    }

    /* Setup the signals to catch */
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = show_stackframe;
    act.sa_flags = SA_SIGINFO;
#    ifdef SA_ONESHOT
    act.sa_flags |= SA_ONESHOT;
#    else
    act.sa_flags |= SA_RESETHAND;
#    endif

    for (tmp = next = prte_signal_string; next != NULL && *next != '\0'; tmp = next + 1) {
        int sig;
        int ret;

        complain = false;
        sig = strtol(tmp, &next, 10);

        /*
         *  If there is no sensible number in the string, exit.
         *  Similarly for any number which is not in the signal-number range
         */
        if (((0 == sig) && (tmp == next)) || (0 > sig) || (_NSIG <= sig)) {
            pmix_show_help("help-prte-util.txt", "stacktrace bad signal", true, prte_signal_string,
                           tmp);
            return PRTE_ERR_SILENT;
        } else if (next == NULL) {
            return PRTE_ERR_BAD_PARAM;
        } else if (':' == *next && 0 == strncasecmp(next, ":complain", 9)) {
            complain = true;
            next += 9;
        } else if (',' != *next && '\0' != *next) {
            return PRTE_ERR_BAD_PARAM;
        }

        /* Just query first */
        ret = sigaction(sig, NULL, &old);
        if (0 != ret) {
            return PRTE_ERR_IN_ERRNO;
        }
        /* Was there something already there? */
        if (SIG_IGN != old.sa_handler && SIG_DFL != old.sa_handler) {
            if (!showed_help && complain) {
                /* JMS This is icky; there is no error message
                   aggregation here so this message may be repeated for
                   every single MPI process... */
                pmix_show_help("help-prte-util.txt", "stacktrace signal override", true, sig, sig,
                               sig, prte_signal_string);
                showed_help = true;
            }
        }

        /* Nope, nothing was there, so put in ours */
        else {
            if (0 != sigaction(sig, &act, NULL)) {
                return PRTE_ERR_IN_ERRNO;
            }
        }
    }

#endif /* PRTE_WANT_PRETTY_PRINT_STACKTRACE */

    return PRTE_SUCCESS;
}
