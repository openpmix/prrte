/*
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 */

#include "prrte_config.h"
#include "src/include/constants.h"

#include "src/util/output.h"
#include "src/util/printf.h"

#include "src/dss/dss.h"
#include "src/hwloc/hwloc-internal.h"

int prrte_hwloc_pack(prrte_buffer_t *buffer, const void *src,
                    int32_t num_vals,
                    prrte_data_type_t type)
{
    /* NOTE: hwloc defines topology_t as a pointer to a struct! */
    hwloc_topology_t t, *tarray  = (hwloc_topology_t*)src;
    int rc, i;
    char *xmlbuffer=NULL;
    int len;
    struct hwloc_topology_support *support;

    for (i=0; i < num_vals; i++) {
        t = tarray[i];

        /* extract an xml-buffer representation of the tree */
        if (0 != prrte_hwloc_base_topology_export_xmlbuffer(t, &xmlbuffer, &len)) {
            return PRRTE_ERROR;
        }

        /* add to buffer */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &xmlbuffer, 1, PRRTE_STRING))) {
            free(xmlbuffer);
            return rc;
        }

        /* cleanup */
        if (NULL != xmlbuffer) {
            free(xmlbuffer);
        }

        /* get the available support - hwloc unfortunately does
         * not include this info in its xml export!
         */
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(t);
        /* pack the discovery support */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, support->discovery,
                                                sizeof(struct hwloc_topology_discovery_support),
                                                PRRTE_BYTE))) {
            return rc;
        }
        /* pack the cpubind support */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, support->cpubind,
                                                sizeof(struct hwloc_topology_cpubind_support),
                                                PRRTE_BYTE))) {
            return rc;
        }
        /* pack the membind support */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, support->membind,
                                                sizeof(struct hwloc_topology_membind_support),
                                                PRRTE_BYTE))) {
            return rc;
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_hwloc_unpack(prrte_buffer_t *buffer, void *dest,
                      int32_t *num_vals,
                      prrte_data_type_t type)
{
    /* NOTE: hwloc defines topology_t as a pointer to a struct! */
    hwloc_topology_t t, *tarray  = (hwloc_topology_t*)dest;
    int rc=PRRTE_SUCCESS, i, cnt, j;
    char *xmlbuffer;
    struct hwloc_topology_support *support;

    for (i=0, j=0; i < *num_vals; i++) {
        /* unpack the xml string */
        cnt=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &xmlbuffer, &cnt, PRRTE_STRING))) {
            goto cleanup;
        }

        /* convert the xml */
        if (0 != hwloc_topology_init(&t)) {
            rc = PRRTE_ERROR;
            free(xmlbuffer);
            goto cleanup;
        }
        if (0 != hwloc_topology_set_xmlbuffer(t, xmlbuffer, strlen(xmlbuffer))) {
            rc = PRRTE_ERROR;
            free(xmlbuffer);
            hwloc_topology_destroy(t);
            goto cleanup;
        }
        free(xmlbuffer);
        /* since we are loading this from an external source, we have to
         * explicitly set a flag so hwloc sets things up correctly
         */
        if (0 != prrte_hwloc_base_topology_set_flags(t, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM, true)) {
            rc = PRRTE_ERROR;
            hwloc_topology_destroy(t);
            goto cleanup;
        }
        /* now load the topology */
        if (0 != hwloc_topology_load(t)) {
            rc = PRRTE_ERROR;
            hwloc_topology_destroy(t);
            goto cleanup;
        }

        /* get the available support - hwloc unfortunately does
         * not include this info in its xml import!
         */
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(t);
        cnt = sizeof(struct hwloc_topology_discovery_support);
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, support->discovery, &cnt, PRRTE_BYTE))) {
            goto cleanup;
        }
        cnt = sizeof(struct hwloc_topology_cpubind_support);
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, support->cpubind, &cnt, PRRTE_BYTE))) {
            goto cleanup;
        }
        cnt = sizeof(struct hwloc_topology_membind_support);
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, support->membind, &cnt, PRRTE_BYTE))) {
            goto cleanup;
        }

        /* pass it back */
        tarray[i] = t;

        /* track the number added */
        j++;
    }

 cleanup:
    *num_vals = j;
    return rc;
}

int prrte_hwloc_copy(hwloc_topology_t *dest, hwloc_topology_t src, prrte_data_type_t type)
{
#ifdef HAVE_HWLOC_TOPOLOGY_DUP
    /* use the hwloc dup function */
    return hwloc_topology_dup(dest, src);
#else
    /* hwloc_topology_dup() was introduced in hwloc v1.8.0.
     * Note that as of March 2017, prrte_hwloc_copy() is not (yet?) used in the code base anywhere. */
    return PRRTE_ERR_NOT_SUPPORTED;
#endif
}

int prrte_hwloc_compare(const hwloc_topology_t topo1,
                       const hwloc_topology_t topo2,
                       prrte_data_type_t type)
{
    hwloc_topology_t t1, t2;
    unsigned d1, d2;
    struct hwloc_topology_support *s1, *s2;
    char *x1=NULL, *x2=NULL;
    int l1, l2;
    int s;

    /* stop stupid compiler warnings */
    t1 = (hwloc_topology_t)topo1;
    t2 = (hwloc_topology_t)topo2;

    /* do something quick first */
    d1 = hwloc_topology_get_depth(t1);
    d2 = hwloc_topology_get_depth(t2);
    if (d1 > d2) {
        return PRRTE_VALUE1_GREATER;
    } else if (d2 > d1) {
        return PRRTE_VALUE2_GREATER;
    }


    /* do the comparison the "cheat" way - get an xml representation
     * of each tree, and strcmp! This will work fine for inventory
     * comparisons, but might not meet the need for comparing topology
     * where we really need to do a tree-wise search so we only compare
     * the things we care about, and ignore stuff like MAC addresses
     */
    if (0 != prrte_hwloc_base_topology_export_xmlbuffer(t1, &x1, &l1)) {
        return PRRTE_EQUAL;
    }
    if (0 != prrte_hwloc_base_topology_export_xmlbuffer(t2, &x2, &l2)) {
        free(x1);
        return PRRTE_EQUAL;
    }

    s = strcmp(x1, x2);
    free(x1);
    free(x2);
    if (s > 0) {
        return PRRTE_VALUE1_GREATER;
    } else if (s < 0) {
        return PRRTE_VALUE2_GREATER;
    }

    /* compare the available support - hwloc unfortunately does
     * not include this info in its xml support!
     */
    if (NULL == (s1 = (struct hwloc_topology_support*)hwloc_topology_get_support(t1)) ||
        NULL == s1->cpubind || NULL == s1->membind) {
        return PRRTE_EQUAL;
    }
    if (NULL == (s2 = (struct hwloc_topology_support*)hwloc_topology_get_support(t2)) ||
        NULL == s2->cpubind || NULL == s2->membind) {
        return PRRTE_EQUAL;
    }
    /* compare the fields we care about */
    if (s1->cpubind->set_thisproc_cpubind != s2->cpubind->set_thisproc_cpubind ||
        s1->cpubind->set_thisthread_cpubind != s2->cpubind->set_thisthread_cpubind ||
        s1->membind->set_thisproc_membind != s2->membind->set_thisproc_membind ||
        s1->membind->set_thisthread_membind != s2->membind->set_thisthread_membind) {
        PRRTE_OUTPUT_VERBOSE((5, prrte_hwloc_base_output,
                             "hwloc:base:compare BINDING CAPABILITIES DIFFER"));
        return PRRTE_VALUE1_GREATER;
    }

    return PRRTE_EQUAL;
}

#define PRRTE_HWLOC_MAX_STRING   2048

static void print_hwloc_obj(char **output, char *prefix,
                            hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t obj2;
    char string[1024], *tmp, *tmp2, *pfx;
    unsigned i;
    struct hwloc_topology_support *support;

    /* print the object type */
    hwloc_obj_type_snprintf(string, 1024, obj, 1);
    prrte_asprintf(&pfx, "\n%s\t", (NULL == prefix) ? "" : prefix);
    prrte_asprintf(&tmp, "%sType: %s Number of child objects: %u%sName=%s",
             (NULL == prefix) ? "" : prefix, string, obj->arity,
             pfx, (NULL == obj->name) ? "NULL" : obj->name);
    if (0 < hwloc_obj_attr_snprintf(string, 1024, obj, pfx, 1)) {
        /* print the attributes */
        prrte_asprintf(&tmp2, "%s%s%s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    /* print the cpusets - apparently, some new HWLOC types don't
     * have cpusets, so protect ourselves here
     */
    if (NULL != obj->cpuset) {
        hwloc_bitmap_snprintf(string, PRRTE_HWLOC_MAX_STRING, obj->cpuset);
        prrte_asprintf(&tmp2, "%s%sCpuset:  %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (HWLOC_OBJ_MACHINE == obj->type) {
        /* root level object - add support values */
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(topo);
        prrte_asprintf(&tmp2, "%s%sBind CPU proc:   %s%sBind CPU thread: %s", tmp, pfx,
                 (support->cpubind->set_thisproc_cpubind) ? "TRUE" : "FALSE", pfx,
                 (support->cpubind->set_thisthread_cpubind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
        prrte_asprintf(&tmp2, "%s%sBind MEM proc:   %s%sBind MEM thread: %s", tmp, pfx,
                 (support->membind->set_thisproc_membind) ? "TRUE" : "FALSE", pfx,
                 (support->membind->set_thisthread_membind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
    }
    prrte_asprintf(&tmp2, "%s%s\n", (NULL == *output) ? "" : *output, tmp);
    free(tmp);
    free(pfx);
    prrte_asprintf(&pfx, "%s\t", (NULL == prefix) ? "" : prefix);
    for (i=0; i < obj->arity; i++) {
        obj2 = obj->children[i];
        /* print the object */
        print_hwloc_obj(&tmp2, pfx, topo, obj2);
    }
    free(pfx);
    if (NULL != *output) {
        free(*output);
    }
    *output = tmp2;
}

int prrte_hwloc_print(char **output, char *prefix, hwloc_topology_t src, prrte_data_type_t type)
{
    hwloc_obj_t obj;
    char *tmp=NULL;

    /* get root object */
    obj = hwloc_get_root_obj(src);
    /* print it */
    print_hwloc_obj(&tmp, prefix, src, obj);
    *output = tmp;
    return PRRTE_SUCCESS;
}
