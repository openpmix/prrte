/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * devinfo -- print the device this process was mapped against.
 *
 *   DEV <rank> <device-id>
 *
 * or, when the job was not mapped by device:
 *
 *   NODEV <rank>
 *
 * Why this exists, rather than reading "--display map" on the head node:
 *
 * PRRTE cannot restrict a process to a device the way it restricts one to a
 * set of CPUs; no such mechanism exists.  The whole value of mapping by
 * device therefore rests on the *process* being able to read its own
 * assignment - and that is a different question from whether the HNP
 * computed one.  The assignment is computed on the HNP, packed into the
 * launch message, unpacked by the daemon that forks the process, and
 * published by that daemon's PMIx server.  "--display map" shows only the
 * first of those four steps.
 *
 * The distinction is not hypothetical.  The assignment was originally
 * recorded as a PRTE_ATTR_GLOBAL proc attribute on the assumption that
 * global attributes travel; they do not - no proc attribute list goes on
 * the wire at all - so it reached the map display and nothing else.  A test
 * that read the map display would have passed throughout.
 *
 * The process also prints the device's distance vector, which is what an
 * application would really do with the assignment: PMIX_DEVICE_ID names one
 * device, and PMIX_DEVICE_DISTANCES lists them all with distances, so a
 * process that cannot find its own id among the distances has been handed an
 * assignment it cannot act on.  Those two answers come from different code
 * paths in PMIx and used to enumerate devices separately.
 */

#include <pmix.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_info_t *dinfo = NULL;
    size_t ndinfo = 0, n, ndev;
    pmix_device_t *dv;
    char *devid = NULL;
    int found = 0;

    (void) argc;
    (void) argv;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "DEVFAIL init %d\n", (int) rc);
        return 1;
    }

    rc = PMIx_Get(&myproc, PMIX_DEVICE_ID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        /* not a device-mapped job, which is a legitimate answer */
        fprintf(stdout, "NODEV %u\n", myproc.rank);
        fflush(stdout);
        PMIx_Finalize(NULL, 0);
        return 0;
    }
    /* The assignment is ALWAYS an array of pmix_device_t, even when it holds
     * a single device.  A reader that special-cased one device would have a
     * path nobody exercised until someone used "ndev". */
    if (PMIX_DATA_ARRAY != val->type || NULL == val->data.darray
        || PMIX_DEVICE != val->data.darray->type) {
        fprintf(stderr, "DEVFAIL %u type %d\n", myproc.rank, (int) val->type);
        PMIX_VALUE_RELEASE(val);
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    dv = (pmix_device_t *) val->data.darray->array;
    ndev = val->data.darray->size;
    if (0 == ndev || NULL == dv[0].uuid) {
        fprintf(stderr, "DEVFAIL %u empty array\n", myproc.rank);
        PMIX_VALUE_RELEASE(val);
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    devid = strdup(dv[0].uuid);
    /* one line per device, so the harness can count them */
    for (n = 0; n < ndev; n++) {
        fprintf(stdout, "DEV %u %zu %s %s\n", myproc.rank, n,
                (NULL == dv[n].uuid) ? "-" : dv[n].uuid,
                (NULL == dv[n].osname) ? "-" : dv[n].osname);
    }
    fprintf(stdout, "DEVN %u %zu\n", myproc.rank, ndev);
    PMIX_VALUE_RELEASE(val);

    /* the assignment has to be findable among the devices this process can
     * see, or it names something the process cannot act on */
    rc = PMIx_Get(&myproc, PMIX_DEVICE_DISTANCES, NULL, 0, &val);
    if (PMIX_SUCCESS == rc && NULL != val && PMIX_DATA_ARRAY == val->type
        && NULL != val->data.darray && PMIX_DEVICE_DIST == val->data.darray->type) {
        pmix_device_distance_t *dd
            = (pmix_device_distance_t *) val->data.darray->array;
        ndinfo = val->data.darray->size;
        for (n = 0; n < ndinfo; n++) {
            if (NULL != dd[n].uuid && 0 == strcmp(dd[n].uuid, devid)) {
                found = 1;
                break;
            }
        }
        fprintf(stdout, "DEVDIST %u %s %d\n", myproc.rank,
                found ? "found" : "MISSING", (int) ndinfo);
    } else {
        /* distances are only generated when the DVM was asked for them */
        fprintf(stdout, "DEVDIST %u none 0\n", myproc.rank);
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    (void) dinfo;

    fflush(stdout);
    free(devid);
    PMIx_Finalize(NULL, 0);
    return 0;
}
