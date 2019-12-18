/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "src/util/fd.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/odls/odls_types.h"

#include "src/mca/rtc/base/base.h"

void prrte_rtc_base_assign(prrte_job_t *jdata)
{
    prrte_rtc_base_selected_module_t *active;

    PRRTE_LIST_FOREACH(active, &prrte_rtc_base.actives, prrte_rtc_base_selected_module_t) {
        if (NULL != active->module->assign) {
            /* give this module a chance to operate on it */
            active->module->assign(jdata);
        }
    }
}

void prrte_rtc_base_set(prrte_job_t *jdata, prrte_proc_t *proc,
                       char ***environ_copy, int error_fd)
{
    prrte_rtc_base_selected_module_t *active;

    PRRTE_LIST_FOREACH(active, &prrte_rtc_base.actives, prrte_rtc_base_selected_module_t) {
        if (NULL != active->module->set) {
            /* give this module a chance to operate on it */
            active->module->set(jdata, proc, environ_copy, error_fd);
        }
    }
}

void prrte_rtc_base_get_avail_vals(prrte_list_t *vals)
{
    prrte_rtc_base_selected_module_t *active;

    PRRTE_LIST_FOREACH(active, &prrte_rtc_base.actives, prrte_rtc_base_selected_module_t) {
        if (NULL != active->module->get_available_values) {
            /* give this module a chance to operate on it */
            active->module->get_available_values(vals);
        }
    }
}


static int write_help_msg(int fd, prrte_odls_pipe_err_msg_t *msg, const char *file,
                          const char *topic, va_list ap)
{
    int ret;
    char *str;

    if (NULL == file || NULL == topic) {
        return PRRTE_ERR_BAD_PARAM;
    }

    str = prrte_show_help_vstring(file, topic, true, ap);

    msg->file_str_len = (int) strlen(file);
    if (msg->file_str_len > PRRTE_ODLS_MAX_FILE_LEN) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    msg->topic_str_len = (int) strlen(topic);
    if (msg->topic_str_len > PRRTE_ODLS_MAX_TOPIC_LEN) {
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        return PRRTE_ERR_BAD_PARAM;
    }
    msg->msg_str_len = (int) strlen(str);

    /* Only keep writing if each write() succeeds */
    if (PRRTE_SUCCESS != (ret = prrte_fd_write(fd, sizeof(*msg), msg))) {
        goto out;
    }
    if (msg->file_str_len > 0 &&
        PRRTE_SUCCESS != (ret = prrte_fd_write(fd, msg->file_str_len, file))) {
        goto out;
    }
    if (msg->topic_str_len > 0 &&
        PRRTE_SUCCESS != (ret = prrte_fd_write(fd, msg->topic_str_len, topic))) {
        goto out;
    }
    if (msg->msg_str_len > 0 &&
        PRRTE_SUCCESS != (ret = prrte_fd_write(fd, msg->msg_str_len, str))) {
        goto out;
    }

 out:
    free(str);
    return ret;
}

int prrte_rtc_base_send_warn_show_help(int fd, const char *file,
                                      const char *topic, ...)
{
    int ret;
    va_list ap;
    prrte_odls_pipe_err_msg_t msg;

    msg.fatal = false;
    msg.exit_status = 0; /* ignored */

    /* Send it */
    va_start(ap, topic);
    ret = write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    return ret;
}

void prrte_rtc_base_send_error_show_help(int fd, int exit_status,
                                        const char *file, const char *topic, ...)
{
    va_list ap;
    prrte_odls_pipe_err_msg_t msg;

    msg.fatal = true;
    msg.exit_status = exit_status;

    /* Send it */
    va_start(ap, topic);
    write_help_msg(fd, &msg, file, topic, ap);
    va_end(ap);

    exit(exit_status);
}

