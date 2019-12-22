/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017      Inria.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#define PRRTE_HWLOC_WANT_SHMEM 1

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "src/class/prrte_list.h"
#include "src/dss/dss_types.h"
#include "src/hwloc/hwloc-internal.h"
#if HWLOC_API_VERSION >= 0x20000
#include "hwloc/shmem.h"
#endif
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"
#include "src/util/fd.h"
#include "src/util/prrte_environ.h"
#include "src/util/path.h"

#include "src/util/show_help.h"
#include "src/util/error_strings.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps_types.h"

#include "src/mca/rtc/base/base.h"
#include "rtc_hwloc.h"

static int init(void);
static void finalize(void);
static void assign(prrte_job_t *jdata);
static void set(prrte_job_t *jdata,
                prrte_proc_t *proc,
                char ***environ_copy,
                int write_fd);

prrte_rtc_base_module_t prrte_rtc_hwloc_module = {
    .init = init,
    .finalize = finalize,
    .assign = assign,
    .set = set
};

#if HWLOC_API_VERSION >= 0x20000
static size_t shmemsize = 0;
static size_t shmemaddr;
static char *shmemfile = NULL;
static int shmemfd = -1;

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          prrte_rtc_hwloc_vm_map_kind_t *kindp);
static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size);
static int find_hole(prrte_rtc_hwloc_vm_hole_kind_t hkind,
                     size_t *addrp,
                     size_t size);
static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result);
#endif

static int init(void)
{
#if HWLOC_API_VERSION >= 0x20000
    int rc;
    bool space_available = false;
    uint64_t amount_space_avail = 0;

    /* ensure we have the topology */
    if (PRRTE_SUCCESS != (rc = prrte_hwloc_base_get_topology())) {
        return rc;
    }

    if (VM_HOLE_NONE == prrte_rtc_hwloc_component.kind) {
        return PRRTE_SUCCESS;
    }

    /* get the size of the topology shared memory segment */
    if (0 != hwloc_shmem_topology_get_length(prrte_hwloc_topology, &shmemsize, 0)) {
        prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                            "%s hwloc topology shmem not available",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        return PRRTE_SUCCESS;
    }

    if (PRRTE_SUCCESS != (rc = find_hole(prrte_rtc_hwloc_component.kind,
                                        &shmemaddr, shmemsize))) {
        /* we couldn't find a hole, so don't use the shmem support */
        if (4 < prrte_output_get_verbosity(prrte_rtc_base_framework.framework_output)) {
            FILE *file = fopen("/proc/self/maps", "r");
            if (file) {
                char line[256];
                prrte_output(0, "%s Dumping /proc/self/maps",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                while (fgets(line, sizeof(line), file) != NULL) {
                    char *end = strchr(line, '\n');
                    if (end) {
                       *end = '\0';
                    }
                    prrte_output(0, "%s", line);
                }
                fclose(file);
            }
        }
        return PRRTE_SUCCESS;
    }
    /* create the shmem file in our session dir so it
     * will automatically get cleaned up */
    prrte_asprintf(&shmemfile, "%s/hwloc.sm", prrte_process_info.jobfam_session_dir);
    /* let's make sure we have enough space for the backing file */
    if (PRRTE_SUCCESS != (rc = enough_space(shmemfile, shmemsize,
                                           &amount_space_avail,
                                           &space_available))) {
        prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                            "%s an error occurred while determining "
                            "whether or not %s could be created for topo shmem.",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        return PRRTE_SUCCESS;
    }
    if (!space_available) {
        if (1 < prrte_output_get_verbosity(prrte_rtc_base_framework.framework_output)) {
            prrte_show_help("help-prrte-rtc-hwloc.txt", "target full", true,
                           shmemfile, prrte_process_info.nodename,
                           (unsigned long)shmemsize,
                           (unsigned long long)amount_space_avail);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PRRTE_SUCCESS;
    }
    /* enough space is available, so create the segment */
    if (-1 == (shmemfd = open(shmemfile, O_CREAT | O_RDWR, 0600))) {
        int err = errno;
        if (1 < prrte_output_get_verbosity(prrte_rtc_base_framework.framework_output)) {
            prrte_show_help("help-prrte-rtc-hwloc.txt", "sys call fail", true,
                           prrte_process_info.nodename,
                           "open(2)", "", strerror(err), err);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PRRTE_SUCCESS;
    }
    /* ensure nobody inherits this fd */
    prrte_fd_set_cloexec(shmemfd);
    /* populate the shmem segment with the topology */
    if (0 != (rc = hwloc_shmem_topology_write(prrte_hwloc_topology, shmemfd, 0,
                                              (void*)shmemaddr, shmemsize, 0))) {
        prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                            "%s an error occurred while writing topology to %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), shmemfile);
        unlink(shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        close(shmemfd);
        shmemfd = -1;
        return PRRTE_SUCCESS;
    }
    prrte_hwloc_shmem_available = true;
#endif

    return PRRTE_SUCCESS;
}

static void finalize(void)
{
#if HWLOC_API_VERSION >= 0x20000
    if (NULL != shmemfile) {
        unlink(shmemfile);
        free(shmemfile);
    }
    if (0 <= shmemfd) {
        close(shmemfd);
    }
#endif
    return;
}

static void assign(prrte_job_t *jdata)
{
#if HWLOC_API_VERSION >= 0x20000
    prrte_list_t *cache;
    prrte_value_t *kv;

    if (VM_HOLE_NONE == prrte_rtc_hwloc_component.kind ||
        NULL == shmemfile) {
        return;
    }
    /* add the shmem address and size to the job-level info that
     * will be provided to the proc upon registration */
    cache = NULL;
    if (!prrte_get_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE, (void**)&cache, PRRTE_PTR) ||
        NULL == cache) {
        cache = PRRTE_NEW(prrte_list_t);
        prrte_set_attribute(&jdata->attributes, PRRTE_JOB_INFO_CACHE, PRRTE_ATTR_LOCAL, cache, PRRTE_PTR);
    }
    prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                        "FILE %s ADDR %lx SIZE %lx", shmemfile,
                        (unsigned long)shmemaddr,
                        (unsigned long)shmemsize);

    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_FILE);
    kv->type = PRRTE_STRING;
    kv->data.string = strdup(shmemfile);
    prrte_list_append(cache, &kv->super);

    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_ADDR);
    kv->type = PRRTE_SIZE;
    kv->data.size = shmemaddr;
    prrte_list_append(cache, &kv->super);

    kv = PRRTE_NEW(prrte_value_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_SIZE);
    kv->type = PRRTE_SIZE;
    kv->data.size = shmemsize;
    prrte_list_append(cache, &kv->super);
#endif
}

static void set(prrte_job_t *jobdat,
                prrte_proc_t *child,
                char ***environ_copy,
                int write_fd)
{
    hwloc_cpuset_t cpuset;
    hwloc_obj_t root;
    prrte_hwloc_topo_data_t *sum;
    prrte_app_context_t *context;
    int rc=PRRTE_ERROR;
    char *msg, *param;
    char *cpu_bitmap;

    prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                        "%s hwloc:set on child %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == child) ? "NULL" : PRRTE_NAME_PRINT(&child->name));

    if (NULL == jobdat || NULL == child) {
        /* nothing for us to do */
        prrte_output_verbose(2, prrte_rtc_base_framework.framework_output,
                            "%s hwloc:set jobdat %s child %s - nothing to do",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            (NULL == jobdat) ? "NULL" : PRRTE_JOBID_PRINT(jobdat->jobid),
                            (NULL == child) ? "NULL" : PRRTE_NAME_PRINT(&child->name));
        return;
    }

    context = (prrte_app_context_t*)prrte_pointer_array_get_item(jobdat->apps, child->app_idx);

    /* Set process affinity, if given */
    cpu_bitmap = NULL;
    if (!prrte_get_attribute(&child->attributes, PRRTE_PROC_CPU_BITMAP, (void**)&cpu_bitmap, PRRTE_STRING) ||
        NULL == cpu_bitmap || 0 == strlen(cpu_bitmap)) {
        /* if the daemon is bound, then we need to "free" this proc */
        if (NULL != prrte_daemon_cores) {
            root = hwloc_get_root_obj(prrte_hwloc_topology);
            if (NULL == root->userdata) {
                prrte_rtc_base_send_warn_show_help(write_fd,
                                                  "help-prrte-odls-default.txt", "incorrectly bound",
                                                  prrte_process_info.nodename, context->app,
                                                  __FILE__, __LINE__);
            }
            sum = (prrte_hwloc_topo_data_t*)root->userdata;
            /* bind this proc to all available processors */
            rc = hwloc_set_cpubind(prrte_hwloc_topology, sum->available, 0);
            /* if we got an error and this wasn't a default binding policy, then report it */
            if (rc < 0  && PRRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                if (errno == ENOSYS) {
                    msg = "hwloc indicates cpu binding not supported";
                } else if (errno == EXDEV) {
                    msg = "hwloc indicates cpu binding cannot be enforced";
                } else {
                    char *tmp;
                    (void)hwloc_bitmap_list_asprintf(&tmp, sum->available);
                    prrte_asprintf(&msg, "hwloc_set_cpubind returned \"%s\" for bitmap \"%s\"",
                             prrte_strerror(rc), tmp);
                    free(tmp);
                }
                if (PRRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                    /* If binding is required, send an error up the pipe (which exits
                       -- it doesn't return). */
                    prrte_rtc_base_send_error_show_help(write_fd, 1, "help-prrte-odls-default.txt",
                                                       "binding generic error",
                                                       prrte_process_info.nodename, context->app, msg,
                                                       __FILE__, __LINE__);
                } else {
                    prrte_rtc_base_send_warn_show_help(write_fd,
                                                      "help-prrte-odls-default.txt", "not bound",
                                                      prrte_process_info.nodename, context->app, msg,
                                                      __FILE__, __LINE__);
                    return;
                }
            }
        }
        if (0 == rc && prrte_hwloc_report_bindings) {
            prrte_output(0, "MCW rank %d is not bound (or bound to all available processors)", child->name.vpid);
            /* avoid reporting it twice */
            (void) prrte_mca_base_var_env_name ("hwloc_base_report_bindings", &param);
            prrte_unsetenv(param, environ_copy);
            free(param);
        }
    } else {
        /* convert the list to a cpuset */
        cpuset = hwloc_bitmap_alloc();
        if (0 != (rc = hwloc_bitmap_list_sscanf(cpuset, cpu_bitmap))) {
            /* See comment above about "This may be a small memory leak" */
            prrte_asprintf(&msg, "hwloc_bitmap_sscanf returned \"%s\" for the string \"%s\"",
                     prrte_strerror(rc), cpu_bitmap);
            if (NULL == msg) {
                msg = "failed to convert bitmap list to hwloc bitmap";
            }
            if (PRRTE_BINDING_REQUIRED(jobdat->map->binding) &&
                PRRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
                /* If binding is required and a binding directive was explicitly
                 * given (i.e., we are not binding due to a default policy),
                 * send an error up the pipe (which exits -- it doesn't return).
                 */
                prrte_rtc_base_send_error_show_help(write_fd, 1, "help-prrte-odls-default.txt",
                                                   "binding generic error",
                                                   prrte_process_info.nodename,
                                                   context->app, msg,
                                                   __FILE__, __LINE__);
            } else {
                prrte_rtc_base_send_warn_show_help(write_fd,
                                                  "help-prrte-odls-default.txt", "not bound",
                                                  prrte_process_info.nodename, context->app, msg,
                                                  __FILE__, __LINE__);
                free(cpu_bitmap);
                return;
            }
        }
        /* bind as specified */
        rc = hwloc_set_cpubind(prrte_hwloc_topology, cpuset, 0);
        /* if we got an error and this wasn't a default binding policy, then report it */
        if (rc < 0  && PRRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            char *tmp = NULL;
            if (errno == ENOSYS) {
                msg = "hwloc indicates cpu binding not supported";
            } else if (errno == EXDEV) {
                msg = "hwloc indicates cpu binding cannot be enforced";
            } else {
                prrte_asprintf(&msg, "hwloc_set_cpubind returned \"%s\" for bitmap \"%s\"",
                         prrte_strerror(rc), cpu_bitmap);
            }
            if (PRRTE_BINDING_REQUIRED(jobdat->map->binding)) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                prrte_rtc_base_send_error_show_help(write_fd, 1, "help-prrte-odls-default.txt",
                                                   "binding generic error",
                                                   prrte_process_info.nodename, context->app, msg,
                                                   __FILE__, __LINE__);
            } else {
                prrte_rtc_base_send_warn_show_help(write_fd,
                                                  "help-prrte-odls-default.txt", "not bound",
                                                  prrte_process_info.nodename, context->app, msg,
                                                  __FILE__, __LINE__);
                if (NULL != tmp) {
                    free(tmp);
                    free(msg);
                }
                return;
            }
            if (NULL != tmp) {
                free(tmp);
                free(msg);
            }
        }
        if (0 == rc && prrte_hwloc_report_bindings) {
            char tmp1[1024], tmp2[1024];
            hwloc_cpuset_t mycpus;
            /* get the cpus we are bound to */
            mycpus = hwloc_bitmap_alloc();
            if (hwloc_get_cpubind(prrte_hwloc_topology,
                                  mycpus,
                                  HWLOC_CPUBIND_PROCESS) < 0) {
                prrte_output(0, "MCW rank %d is not bound",
                            child->name.vpid);
            } else {
                if (PRRTE_ERR_NOT_BOUND == prrte_hwloc_base_cset2str(tmp1, sizeof(tmp1), prrte_hwloc_topology, mycpus)) {
                    prrte_output(0, "MCW rank %d is not bound (or bound to all available processors)", child->name.vpid);
                } else {
                    prrte_hwloc_base_cset2mapstr(tmp2, sizeof(tmp2), prrte_hwloc_topology, mycpus);
                    prrte_output(0, "MCW rank %d bound to %s: %s",
                                child->name.vpid, tmp1, tmp2);
                }
            }
            hwloc_bitmap_free(mycpus);
            /* avoid reporting it twice */
            (void) prrte_mca_base_var_env_name ("hwloc_base_report_bindings", &param);
            prrte_unsetenv(param, environ_copy);
            free(param);
        }
        /* set memory affinity policy - if we get an error, don't report
         * anything unless the user actually specified the binding policy
         */
        rc = prrte_hwloc_base_set_process_membind_policy();
        if (PRRTE_SUCCESS != rc  && PRRTE_BINDING_POLICY_IS_SET(jobdat->map->binding)) {
            if (errno == ENOSYS) {
                msg = "hwloc indicates memory binding not supported";
            } else if (errno == EXDEV) {
                msg = "hwloc indicates memory binding cannot be enforced";
            } else {
                msg = "failed to bind memory";
            }
            if (PRRTE_HWLOC_BASE_MBFA_ERROR == prrte_hwloc_base_mbfa) {
                /* If binding is required, send an error up the pipe (which exits
                   -- it doesn't return). */
                prrte_rtc_base_send_error_show_help(write_fd, 1, "help-prrte-odls-default.txt",
                                                   "memory binding error",
                                                   prrte_process_info.nodename, context->app, msg,
                                                   __FILE__, __LINE__);
            } else {
                prrte_rtc_base_send_warn_show_help(write_fd,
                                                  "help-prrte-odls-default.txt", "memory not bound",
                                                  prrte_process_info.nodename, context->app, msg,
                                                  __FILE__, __LINE__);
                free(cpu_bitmap);
                return;
            }
        }
    }
    if (NULL != cpu_bitmap) {
        free(cpu_bitmap);
    }
}

#if HWLOC_API_VERSION >= 0x20000

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          prrte_rtc_hwloc_vm_map_kind_t *kindp)
{
    const char *tmp = line, *next;
    unsigned long value;

    /* "beginaddr-endaddr " */
    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PRRTE_ERROR;
    }

    *beginp = (unsigned long) value;

    if (*next != '-') {
        return PRRTE_ERROR;
    }

     tmp = next + 1;

    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PRRTE_ERROR;
    }
    *endp = (unsigned long) value;
    tmp = next;

    if (*next != ' ') {
        return PRRTE_ERROR;
    }
    tmp = next + 1;

    /* look for ending absolute path */
    next = strchr(tmp, '/');
    if (next) {
        *kindp = VM_MAP_FILE;
    } else {
        /* look for ending special tag [foo] */
        next = strchr(tmp, '[');
        if (next) {
            if (!strncmp(next, "[heap]", 6)) {
                *kindp = VM_MAP_HEAP;
            } else if (!strncmp(next, "[stack]", 7)) {
                *kindp = VM_MAP_STACK;
            } else {
                char *end;
                if ((end = strchr(next, '\n')) != NULL) {
                    *end = '\0';
                }
                prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                                    "Found special VMA \"%s\" before stack", next);
                *kindp = VM_MAP_OTHER;
            }
        } else {
            *kindp = VM_MAP_ANONYMOUS;
        }
    }

    return PRRTE_SUCCESS;
}

#define ALIGN2MB (2*1024*1024UL)

static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size)
{
    unsigned long aligned;
    unsigned long middle = holebegin+holesize/2;

    prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                        "looking in hole [0x%lx-0x%lx] size %lu (%lu MB) for %lu (%lu MB)\n",
                        holebegin, holebegin+holesize, holesize, holesize>>20, size, size>>20);

    if (holesize < size) {
        return PRRTE_ERROR;
    }

    /* try to align the middle of the hole on 64MB for POWER's 64k-page PMD */
    #define ALIGN64MB (64*1024*1024UL)
    aligned = (middle + ALIGN64MB) & ~(ALIGN64MB-1);
    if (aligned + size <= holebegin + holesize) {
        prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                            "aligned [0x%lx-0x%lx] (middle 0x%lx) to 0x%lx for 64MB\n",
                            holebegin, holebegin+holesize, middle, aligned);
        prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                            " there are %lu MB free before and %lu MB free after\n",
                            (aligned-holebegin)>>20, (holebegin+holesize-aligned-size)>>20);

        *addrp = aligned;
        return PRRTE_SUCCESS;
    }

    /* try to align the middle of the hole on 2MB for x86 PMD */
    aligned = (middle + ALIGN2MB) & ~(ALIGN2MB-1);
    if (aligned + size <= holebegin + holesize) {
        prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                            "aligned [0x%lx-0x%lx] (middle 0x%lx) to 0x%lx for 2MB\n",
                            holebegin, holebegin+holesize, middle, aligned);
        prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                            " there are %lu MB free before and %lu MB free after\n",
                            (aligned-holebegin)>>20, (holebegin+holesize-aligned-size)>>20);
        *addrp = aligned;
        return PRRTE_SUCCESS;
    }

    /* just use the end of the hole */
    *addrp = holebegin + holesize - size;
    prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                        "using the end of hole starting at 0x%lx\n", *addrp);
    prrte_output_verbose(80, prrte_rtc_base_framework.framework_output,
                        " there are %lu MB free before\n", (*addrp-holebegin)>>20);
    return PRRTE_SUCCESS;
}

static int find_hole(prrte_rtc_hwloc_vm_hole_kind_t hkind,
                     size_t *addrp, size_t size)
{
    unsigned long biggestbegin = 0;
    unsigned long biggestsize = 0;
    unsigned long prevend = 0;
    prrte_rtc_hwloc_vm_map_kind_t prevmkind = VM_MAP_OTHER;
    int in_libs = 0;
    FILE *file;
    char line[96];

    file = fopen("/proc/self/maps", "r");
    if (!file) {
        return PRRTE_ERROR;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long begin=0, end=0;
        prrte_rtc_hwloc_vm_map_kind_t mkind=VM_MAP_OTHER;

        if (!parse_map_line(line, &begin, &end, &mkind)) {
            prrte_output_verbose(90, prrte_rtc_base_framework.framework_output,
                                "found %s from 0x%lx to 0x%lx\n",
                                mkind == VM_MAP_HEAP ? "HEAP" :
                                mkind == VM_MAP_STACK ? "STACK" :
                                mkind == VM_MAP_OTHER ? "OTHER" :
                                mkind == VM_MAP_FILE ? "FILE" :
                                mkind == VM_MAP_ANONYMOUS ? "ANON" : "unknown",
                                begin, end);

            switch (hkind) {
                case VM_HOLE_BEGIN:
                    fclose(file);
                    return use_hole(0, begin, addrp, size);

                case VM_HOLE_AFTER_HEAP:
                    if (prevmkind == VM_MAP_HEAP && mkind != VM_MAP_HEAP) {
                        /* only use HEAP when there's no other HEAP after it
                         * (there can be several of them consecutively).
                         */
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_BEFORE_STACK:
                    if (mkind == VM_MAP_STACK) {
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_IN_LIBS:
                    /* see if we are between heap and stack */
                    if (prevmkind == VM_MAP_HEAP) {
                        in_libs = 1;
                    }
                    if (mkind == VM_MAP_STACK) {
                        in_libs = 0;
                    }
                    if (!in_libs) {
                        /* we're not in libs, ignore this entry */
                        break;
                    }
                    /* we're in libs, consider this entry for searching the biggest hole below */
                    /* fallthrough */

                case VM_HOLE_BIGGEST:
                    if (begin-prevend > biggestsize) {
                        prrte_output_verbose(90, prrte_rtc_base_framework.framework_output,
                                            "new biggest 0x%lx - 0x%lx = %lu (%lu MB)\n",
                                            prevend, begin, begin-prevend, (begin-prevend)>>20);
                        biggestbegin = prevend;
                        biggestsize = begin-prevend;
                    }
                    break;

                    default:
                        assert(0);
            }
        }

        while (!strchr(line, '\n')) {
            if (!fgets(line, sizeof(line), file)) {
                goto done;
            }
        }

        if (mkind == VM_MAP_STACK) {
          /* Don't go beyond the stack. Other VMAs are special (vsyscall, vvar, vdso, etc),
           * There's no spare room there. And vsyscall is even above the userspace limit.
           */
          break;
        }

        prevend = end;
        prevmkind = mkind;

    }

  done:
    fclose(file);
    if (hkind == VM_HOLE_IN_LIBS || hkind == VM_HOLE_BIGGEST) {
        return use_hole(biggestbegin, biggestsize, addrp, size);
    }

    return PRRTE_ERROR;
}

static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result)
{
    uint64_t avail = 0;
    size_t fluff = (size_t)(.05 * space_req);
    bool enough = false;
    char *last_sep = NULL;
    /* the target file name is passed here, but we need to check the parent
     * directory. store it so we can extract that info later. */
    char *target_dir = strdup(filename);
    int rc;

    if (NULL == target_dir) {
        rc = PRRTE_ERR_OUT_OF_RESOURCE;
        goto out;
    }
    /* get the parent directory */
    last_sep = strrchr(target_dir, PRRTE_PATH_SEP[0]);
    *last_sep = '\0';
    /* now check space availability */
    if (PRRTE_SUCCESS != (rc = prrte_path_df(target_dir, &avail))) {
        PRRTE_OUTPUT_VERBOSE(
            (70, prrte_rtc_base_framework.framework_output,
             "WARNING: prrte_path_df failure!")
        );
        goto out;
    }
    /* do we have enough space? */
    if (avail >= space_req + fluff) {
        enough = true;
    }
    else {
        PRRTE_OUTPUT_VERBOSE(
            (70, prrte_rtc_base_framework.framework_output,
             "WARNING: not enough space on %s to meet request!"
             "available: %"PRIu64 "requested: %lu", target_dir,
             avail, (unsigned long)space_req + fluff)
        );
    }

out:
    if (NULL != target_dir) {
        free(target_dir);
    }
    *result = enough;
    *space_avail = avail;
    return rc;
}
#endif
