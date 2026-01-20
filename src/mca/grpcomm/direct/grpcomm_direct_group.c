/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

static void group(int sd, short args, void *cbdata);

static prte_grpcomm_group_t *get_tracker(prte_grpcomm_direct_group_signature_t *sig, bool create);

static int create_dmns(prte_grpcomm_direct_group_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);

static int pack_signature(pmix_data_buffer_t *buf,
                          prte_grpcomm_direct_group_signature_t *sig);

static int unpack_signature(pmix_data_buffer_t *buf,
                            prte_grpcomm_direct_group_signature_t **sig);

int prte_grpcomm_direct_group(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:group %s for \"%s\" with %lu procs",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PMIx_Group_operation_string(op), grpid, nprocs));

    cd = PMIX_NEW(prte_pmix_grp_caddy_t);
    cd->op = op;
    cd->grpid = strdup(grpid);
    cd->procs = procs;
    cd->nprocs = nprocs;
    cd->directives = directives;
    cd->ndirs = ndirs;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, group, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

static void group(int sd, short args, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    prte_grpcomm_direct_group_signature_t sig;
    prte_grpcomm_group_t *coll;
    size_t i;
    pmix_data_buffer_t *relay;
    pmix_status_t rc, st = PMIX_SUCCESS;
    int timeout = 0;
    void *endpts, *grpinfo;
    pmix_data_array_t darray;
    pmix_info_t *info;
    size_t ninfo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    /* compute the signature of this collective */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_direct_group_signature_t);
    sig.groupID = strdup(cd->grpid);
    if (NULL != cd->procs) {
        sig.nmembers = cd->nprocs;
        PMIX_PROC_CREATE(sig.members, sig.nmembers);
        memcpy(sig.members, cd->procs, sig.nmembers * sizeof(pmix_proc_t));
    } else {
        // if no procs were given, then this must be a follower
        sig.follower = true;
    }
    sig.op = cd->op;

    // setup to track endpts and grpinfo
    endpts = PMIx_Info_list_start();
    grpinfo = PMIx_Info_list_start();

    /* check the directives */
    for (i = 0; i < cd->ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            sig.assignID = PMIX_INFO_TRUE(&cd->directives[i]);

#ifdef PMIX_GROUP_BOOTSTRAP
        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_GROUP_BOOTSTRAP)) {
            PMIX_VALUE_GET_NUMBER(rc, &cd->directives[i].value, sig.bootstrap, size_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&sig);
            }
#endif

        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            PMIX_VALUE_GET_NUMBER(rc, &cd->directives[i].value, st, pmix_status_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&sig);
                goto error;
            }

        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &cd->directives[i].value, timeout, int);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&sig);
                goto error;
            }

        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_GROUP_ADD_MEMBERS)) {
            // there is only one of these as it is aggregated by the
            // PMIx server library
            sig.addmembers = (pmix_proc_t*)cd->directives[i].value.data.darray->array;
            sig.naddmembers = cd->directives[i].value.data.darray->size;

        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_GROUP_INFO)) {
            rc = PMIx_Info_list_xfer(grpinfo, &cd->directives[i]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }

        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_PROC_DATA)) {
            rc = PMIx_Info_list_xfer(endpts, &cd->directives[i]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }

#ifdef PMIX_GROUP_FINAL_MEMBERSHIP_ORDER
        } else if (PMIX_CHECK_KEY(&cd->directives[i], PMIX_GROUP_FINAL_MEMBERSHIP_ORDER)) {
            sig.final_order = (pmix_proc_t*)cd->directives[i].value.data.darray->array;
            sig.nfinal = cd->directives[i].value.data.darray->size;
#endif
        }
    }

    /* create a tracker for this operation */
    if (NULL == (coll = get_tracker(&sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(cd);
        PMIX_DESTRUCT(&sig);
        return;
    }
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    // create the relay buffer
    PMIX_DATA_BUFFER_CREATE(relay);

    /* pack the signature */
    rc = pack_signature(relay, &sig);
    // protect the inbound directives
    sig.addmembers = NULL;
    sig.naddmembers = 0;
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    // pack the local collective status
    rc = PMIx_Data_pack(NULL, relay, &st, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    // pack any timeout directive
    rc = PMIx_Data_pack(NULL, relay, &timeout, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    if (PMIX_GROUP_CONSTRUCT == sig.op) {
        // pack any group info
        PMIx_Info_list_convert(grpinfo, &darray);
        info = (pmix_info_t*)darray.array;
        ninfo = darray.size;
        rc = PMIx_Data_pack(NULL, relay, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        if (0 < ninfo) {
            rc = PMIx_Data_pack(NULL, relay, info, ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(relay);
                PMIX_DESTRUCT(&sig);
                goto error;
            }
        }
        PMIX_DATA_ARRAY_DESTRUCT(&darray);

        // pack any endpts
        PMIx_Info_list_convert(endpts, &darray);
        info = (pmix_info_t*)darray.array;
        ninfo = darray.size;
        rc = PMIx_Data_pack(NULL, relay, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        if (0 < ninfo) {
            rc = PMIx_Data_pack(NULL, relay, info, ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(relay);
                PMIX_DESTRUCT(&sig);
                goto error;
            }
        }
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
    }
    PMIx_Info_list_release(grpinfo);
    PMIx_Info_list_release(endpts);

    /* if this is a bootstrap operation, send it directly to the HNP */
    if (coll->bootstrap) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct:grp bootstrap sending %lu bytes to HNP",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), relay->bytes_used));

        PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, relay,
                      PRTE_RML_TAG_GROUP);
        if (PRTE_SUCCESS != rc) {
            PMIX_RELEASE(relay);
            rc = prte_pmix_convert_rc(rc);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        PMIX_DESTRUCT(&sig);
        return;
    }
    PMIX_DESTRUCT(&sig);

    /* send this to ourselves for processing */
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:grp sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, relay,
                  PRTE_RML_TAG_GROUP);
    if (PRTE_SUCCESS != rc) {
        PMIX_RELEASE(relay);
        rc = prte_pmix_convert_rc(rc);
        goto error;
    }
    return;

error:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(cd);
}

void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, timeout;
    size_t m, n, ninfo, nfinal = 0, nendpts, ngrpinfo;
    pmix_proc_t *finalmembership = NULL;
    bool found;
    pmix_list_t nmlist;
    prte_namelist_t *nm;
    pmix_data_array_t darray;
    pmix_status_t st;
    pmix_info_t *info = NULL, *endpts, *grpinfo = NULL;
    prte_grpcomm_direct_group_signature_t *sig = NULL;
    pmix_data_buffer_t *reply;
    prte_grpcomm_group_t *coll;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct grp recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)));

    // empty buffer indicates lost connection
    if (NULL == buffer || NULL == buffer->unpack_ptr) {
        PMIX_ERROR_LOG(PMIX_ERR_EMPTY);
        return;
    }

    /* unpack the signature */
    rc = unpack_signature(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }

    // unpack the local collective status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    if (PMIX_SUCCESS != st &&
        PMIX_SUCCESS == coll->status) {
        coll->status = st;
    }

    // unpack any timeout directive
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &timeout, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    if (coll->timeout < timeout) {
        coll->timeout = timeout;
    }


    if (PMIX_GROUP_CONSTRUCT == sig->op) {
        /* unpack the number of group infos */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &ngrpinfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(sig);
            return;
        }
        if (0 < ngrpinfo) {
            PMIX_INFO_CREATE(grpinfo, ngrpinfo);
            cnt = ngrpinfo;
            rc = PMIx_Data_unpack(NULL, buffer, grpinfo, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
                PMIX_RELEASE(sig);
                return;
            }
            for (n=0; n < ngrpinfo; n++) {
                rc = PMIx_Info_list_xfer(coll->grpinfo, &grpinfo[n]);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_FREE(grpinfo, ngrpinfo);
        }

        /* unpack the number of endpoints */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &nendpts, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (NULL != grpinfo) {
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
            }
            PMIX_RELEASE(sig);
            return;
        }
        if (0 < nendpts) {
            PMIX_INFO_CREATE(endpts, nendpts);
            cnt = nendpts;
            rc = PMIx_Data_unpack(NULL, buffer, endpts, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (NULL != grpinfo) {
                    PMIX_INFO_FREE(grpinfo, ngrpinfo);
                }
                PMIX_INFO_FREE(endpts, nendpts);
                PMIX_RELEASE(sig);
                return;
            }
            for (n=0; n < nendpts; n++) {
                rc = PMIx_Info_list_xfer(coll->endpts, &endpts[n]);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_FREE(endpts, nendpts);
        }
    }

    /* track procs reported for collective */
    if (0 < sig->bootstrap) {
        // this came from a bootstrap leader
        coll->nleaders_reported++;
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv leader nrep %d of %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nleaders_reported,
                             (int) coll->nleaders));
    } else if (sig->follower) {
        // came from a bootstrap follower
        coll->nfollowers_reported++;
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv follower nrep %d of %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nfollowers_reported,
                             (int) coll->nfollowers));
    } else {
        // group collective op
        coll->nreported++;
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv nexpected %d nrep %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nexpected,
                             (int) coll->nreported));
    }


    /* see if everyone has reported */
    if ((coll->bootstrap && (coll->nleaders_reported == coll->nleaders &&
                             coll->nfollowers_reported == coll->nfollowers)) ||
        (!coll->bootstrap && coll->nreported == coll->nexpected)) {

        if (PRTE_PROC_IS_MASTER) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct group HNP reports complete for %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID));

            /* the allgather is complete - send the xcast */
            if (PMIX_GROUP_CONSTRUCT == sig->op) {
                /* if we were asked to provide a context id, do so */
                if (coll->sig->assignID) {
                    coll->sig->ctxid = prte_grpcomm_base.context_id;
                    --prte_grpcomm_base.context_id;
                    coll->sig->ctxid_assigned = true;
                }

                // construct the final membership
                PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                // sadly, an exhaustive search
                for (m=0; m < coll->sig->nmembers; m++) {
                    found = false;
                    PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                        if (PMIX_CHECK_PROCID(&coll->sig->members[m], &nm->name)) {
                            // if the new one is rank=WILDCARD, then ensure
                            // we keep it as wildcard
                            if (PMIX_RANK_WILDCARD == coll->sig->members[m].rank) {
                                nm->name.rank = PMIX_RANK_WILDCARD;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &coll->sig->members[m], sizeof(pmix_proc_t));
                        pmix_list_append(&nmlist, &nm->super);
                    }
                }
                // now check any added members
                for (m=0; m < coll->sig->naddmembers; m++) {
                    found = false;
                    PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                        if (PMIX_CHECK_PROCID(&coll->sig->addmembers[m], &nm->name)) {
                            // if the new one is rank=WILDCARD, then ensure
                            // we keep it as wildcard
                            if (PMIX_RANK_WILDCARD == coll->sig->addmembers[m].rank) {
                                nm->name.rank = PMIX_RANK_WILDCARD;
                            }
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &coll->sig->addmembers[m], sizeof(pmix_proc_t));
                        pmix_list_append(&nmlist, &nm->super);
                    }
                }
                // create the full membership array
                nfinal = pmix_list_get_size(&nmlist);
                PMIX_PROC_CREATE(finalmembership, nfinal);
                m = 0;
                PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                    memcpy(&finalmembership[m], &nm->name, sizeof(pmix_proc_t));
                    ++m;
                }
                PMIX_LIST_DESTRUCT(&nmlist);

                // if they gave us a final order, then sort the final membership
                // accordingly. Note that order entries that consist of nspace,wildcard
                // indicate that all participants from the given nspace should be
                // included in the final membership at that point - it does NOT mean
                // that all procs from that nspace are included in the final membership
                if (NULL != coll->sig->final_order) {
                    PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                    for (m=0; m < coll->sig->nfinal; m++) {
                        // search the array of final members to capture those that match
                        for (n=0; n < nfinal; n++) {
                            if (PMIX_CHECK_PROCID(&coll->sig->final_order[m], &finalmembership[n])) {
                                // add this proc to the final list
                                nm = PMIX_NEW(prte_namelist_t);
                                memcpy(&nm->name, &finalmembership[n], sizeof(pmix_proc_t));
                                pmix_list_append(&nmlist, &nm->super);
                                // the final order may have included rank=wildcard - if so,
                                // then we have to continue
                                if (PMIX_RANK_WILDCARD != coll->sig->final_order[m].rank) {
                                    // nope - can only match once
                                    break;
                                }
                            }
                        }
                    }
                    // did we lose anyone?
                    if (nfinal != pmix_list_get_size(&nmlist)) {
                        pmix_show_help("help-prte-runtime.txt", "bad-final-order", true);
                        coll->status = PMIX_ERR_BAD_PARAM;
                        goto answer;
                    }
                    // just overwrite the final array
                    m = 0;
                    PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                        memcpy(&finalmembership[m], &nm->name, sizeof(pmix_proc_t));
                        ++m;
                    }
                    PMIX_LIST_DESTRUCT(&nmlist);
 
                    // zero out the final order cache - no need to send it around
                    PMIX_PROC_FREE(coll->sig->final_order, coll->sig->nfinal);
                    coll->sig->final_order = NULL;
                    coll->sig->nfinal = 0;

                } else {
                     /* sort the procs so everyone gets the same order */
                    qsort(finalmembership, nfinal, sizeof(pmix_proc_t), pmix_util_compare_proc);
                }
            }

answer:
            // CONSTRUCT THE RELEASE MESSAGE
            PMIX_DATA_BUFFER_CREATE(reply);

            /* pack the signature */
            rc = pack_signature(reply, coll->sig);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                PMIX_PROC_FREE(finalmembership, nfinal);
                return;
            }
            /* pack the status */
            rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                PMIX_PROC_FREE(finalmembership, nfinal);
                return;
            }

            if (PMIX_GROUP_CONSTRUCT == sig->op) {
                // pack the final membership
                rc = PMIx_Data_pack(NULL, reply, &nfinal, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_RELEASE(sig);
                    PMIX_PROC_FREE(finalmembership, nfinal);
                    return;
                }
                if (0 < nfinal) {
                    rc = PMIx_Data_pack(NULL, reply, finalmembership, nfinal, PMIX_PROC);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(reply);
                        PMIX_RELEASE(sig);
                        return;
                    }
                    PMIX_PROC_FREE(finalmembership, nfinal);
                }

                // pack any group info
                PMIx_Info_list_convert(coll->grpinfo, &darray);
                info = (pmix_info_t*)darray.array;
                ninfo = darray.size;
                rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_RELEASE(sig);
                    return;
                }
                if (0 < ninfo) {
                   rc =  PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(reply);
                        PMIX_RELEASE(sig);
                        return;
                    }
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);

                // pack any endpts
                PMIx_Info_list_convert(coll->endpts, &darray);
                info = (pmix_info_t*)darray.array;
                ninfo = darray.size;
                rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_RELEASE(sig);
                    return;
                }
                if (0 < ninfo) {
                    rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(reply);
                        PMIX_RELEASE(sig);
                        return;
                    }
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);
            }

            /* send the release via xcast */
            (void) prte_grpcomm.xcast(PRTE_RML_TAG_GROUP_RELEASE, reply);

        } else {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct allgather rollup complete - sending to %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));

            // setup to relay our rollup results
            PMIX_DATA_BUFFER_CREATE(reply);

            /* pack the signature */
            rc = pack_signature(reply, coll->sig);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                return;
            }

            // pack the local collective status
            rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                return;
            }

            // pack any timeout directive
            rc = PMIx_Data_pack(NULL, reply, &coll->timeout, 1, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                return;
            }

            if (PMIX_GROUP_CONSTRUCT == sig->op) {
                // pack any group info
                PMIx_Info_list_convert(coll->grpinfo, &darray);
                info = (pmix_info_t*)darray.array;
                ninfo = darray.size;
                rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_RELEASE(sig);
                    return;
                }
                if (0 < ninfo) {
                    rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(reply);
                        PMIX_RELEASE(sig);
                        return;
                    }
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);

                // pack any endpts
                PMIx_Info_list_convert(coll->endpts, &darray);
                info = (pmix_info_t*)darray.array;
                ninfo = darray.size;
                rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_RELEASE(sig);
                    return;
                }
                if (0 < ninfo) {
                    rc =PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DATA_BUFFER_RELEASE(reply);
                        PMIX_RELEASE(sig);
                        return;
                    }
                }
                PMIX_DATA_ARRAY_DESTRUCT(&darray);
            }

            /* send the info to our parent */
            PRTE_RML_SEND(rc, PRTE_PROC_MY_PARENT->rank, reply,
                          PRTE_RML_TAG_GROUP);
            if (PRTE_SUCCESS != rc) {
                PRTE_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_RELEASE(sig);
                return;
            }
        }
    }
    PMIX_RELEASE(sig);
}

static void relcb(void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    PMIX_RELEASE(cd);
}

static void lkopcbfunc(int status, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    PRTE_HIDE_UNUSED_PARAMS(status);

    cd->lock.status = status;
    PMIX_WAKEUP_THREAD(&cd->lock);
}

static void find_delete_tracker(prte_grpcomm_direct_group_signature_t *sig)
{
    prte_grpcomm_group_t *coll;

    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.group_ops, prte_grpcomm_group_t) {
        // must match groupID's
        if (0 == strcmp(sig->groupID, coll->sig->groupID)) {
            pmix_list_remove_item(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);
            PMIX_RELEASE(coll);
            return;
        }
    }
}

void prte_grpcomm_direct_grp_release(int status, pmix_proc_t *sender,
                                     pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag, void *cbdata)
{
    prte_grpcomm_group_t *coll;
    prte_grpcomm_direct_group_signature_t *sig = NULL;
    prte_pmix_grp_caddy_t cd2, *cd;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS, st = PMIX_SUCCESS;
    pmix_proc_t *finalmembership = NULL;
    size_t nfinal = 0;
    size_t nendpts = 0;
    size_t ngrpinfo = 0;
    size_t n;
    pmix_data_array_t darray;
    pmix_info_t *grpinfo = NULL;
    pmix_info_t *endpts = NULL;
    prte_pmix_server_pset_t *pset;
    void *ilist, *nlist;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    PMIX_ACQUIRE_OBJECT(cd);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group release recvd",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    // unpack the signature
    rc = unpack_signature(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* check for the tracker - okay if not found, it just
     * means that we had no local participants */
    coll = get_tracker(sig, false);

    // unpack the status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        st = rc;
    }

    /* if this was a destruct operation, then there is nothing
     * further to unpack */
    if (PMIX_GROUP_DESTRUCT == sig->op) {
        /* find this group ID on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, prte_pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, sig->groupID)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        if (NULL != coll && NULL != coll->cbfunc) {
            /* return to the local procs in the collective */
            coll->cbfunc(st, NULL, 0, coll->cbdata, NULL, NULL);
        }
        // remove the tracker, if found
        find_delete_tracker(sig);
        PMIX_RELEASE(sig);
        return;
    }

    // setup to cache info
    ilist = PMIx_Info_list_start();
    nlist = PMIx_Info_list_start();

    // must be a construct operation - continue unpacking
    if (PMIX_SUCCESS != st) {
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }

    if (sig->ctxid_assigned) {
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_CONTEXT_ID, &sig->ctxid, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        PMIX_INFO_LIST_ADD(rc, nlist, PMIX_GROUP_CONTEXT_ID, &sig->ctxid, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
    }

    // unpack the final membership
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nfinal, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        st = rc;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < nfinal) {
        PMIX_PROC_CREATE(finalmembership, nfinal);
        cnt = nfinal;
        rc = PMIx_Data_unpack(NULL, buffer, finalmembership, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        // pass back the final group membership
        darray.type = PMIX_PROC;
        darray.array = finalmembership;
        darray.size = nfinal;
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(rc, nlist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        PMIX_PROC_FREE(finalmembership, nfinal);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
    }

    // unpack group info
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ngrpinfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        st = rc;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < ngrpinfo) {
        PMIX_INFO_CREATE(grpinfo, ngrpinfo);
        cnt = ngrpinfo;
        rc = PMIx_Data_unpack(NULL, buffer, grpinfo, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            PMIX_INFO_FREE(grpinfo, ngrpinfo);
            goto notify;
        }
        // transfer them to both lists
        for (n=0; n < ngrpinfo; n++) {
            rc = PMIx_Info_list_add_value(ilist, PMIX_GROUP_INFO, &grpinfo[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
                goto notify;
            }
            rc = PMIx_Info_list_add_value(nlist, PMIX_GROUP_INFO, &grpinfo[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
                goto notify;
            }
        }
        PMIX_INFO_FREE(grpinfo, ngrpinfo);
    }


    // unpack endpts
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nendpts, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        st = rc;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < nendpts) {
        PMIX_INFO_CREATE(endpts, nendpts);
        cnt = nendpts;
        rc = PMIx_Data_unpack(NULL, buffer, endpts, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            PMIX_INFO_FREE(endpts, nendpts);
            goto notify;
        }
        // transfer them to both lists
        for (n=0; n < nendpts; n++) {
            rc = PMIx_Info_list_add_value(ilist, PMIX_GROUP_ENDPT_DATA, &endpts[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                PMIX_INFO_FREE(endpts, nendpts);
                goto notify;
            }
            rc = PMIx_Info_list_add_value(nlist, PMIX_GROUP_ENDPT_DATA, &endpts[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                PMIX_INFO_FREE(endpts, nendpts);
                goto notify;
            }
        }
        PMIX_INFO_FREE(endpts, nendpts);
    }

    // PRRTE automatically ensures that all daemons register all jobs
    // with their local PMIx server, regardless of whether or not
    // that daemon hosts any local clients of that job. So we
    // do not need to collect/pass job data for participants
    // in the group construct

    // pass the information down to the PMIx server
    PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
    PMIX_CONSTRUCT(&cd2, prte_pmix_grp_caddy_t);
    cd2.info = (pmix_info_t*)darray.array;
    cd2.ninfo = darray.size;
    PMIX_INFO_LIST_RELEASE(ilist);

    rc = PMIx_server_register_resources(cd2.info, cd2.ninfo, lkopcbfunc, &cd2);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&cd2.lock);
        rc = cd2.lock.status;
    }
    PMIX_DESTRUCT(&cd2);

    if (PMIX_SUCCESS == st) {
       /* add it to our list of known groups */
        pset = PMIX_NEW(prte_pmix_server_pset_t);
        pset->name = strdup(sig->groupID);
        if (NULL != finalmembership) {
            pset->num_members = nfinal;
            PMIX_PROC_CREATE(pset->members, pset->num_members);
            memcpy(pset->members, finalmembership, nfinal * sizeof(pmix_proc_t));
        }
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
    }

notify:
    // regardless of prior error, we MUST notify any pending clients
    // so they don't hang

    if (NULL != coll && NULL != coll->cbfunc) {
        // service the procs that are part of the collective

        // convert for returning to PMIx server library
        cd = PMIX_NEW(prte_pmix_grp_caddy_t);
        if (PMIX_SUCCESS == st) {
            PMIX_INFO_LIST_CONVERT(rc, nlist, &darray);
            if (PMIX_SUCCESS != rc && PMIX_ERR_EMPTY != rc) {
                PMIX_ERROR_LOG(rc);
            }
            cd->info = (pmix_info_t*)darray.array;
            cd->ninfo = darray.size;
        }

        /* return to the PMIx server library for relay to
         * local procs in the operation */
        coll->cbfunc(st, cd->info, cd->ninfo, coll->cbdata, relcb, (void*)cd);
    }

    if (0 < nendpts) {
        PMIX_INFO_FREE(endpts, nendpts);
    }
    if (0 < ngrpinfo) {
        PMIX_INFO_FREE(grpinfo, ngrpinfo);
    }
    PMIX_INFO_LIST_RELEASE(nlist);
    // remove this collective from our tracker
    find_delete_tracker(sig);
    PMIX_RELEASE(sig);
}


static prte_grpcomm_group_t *get_tracker(prte_grpcomm_direct_group_signature_t *sig,
                                         bool create)
{
    prte_grpcomm_group_t *coll;
    int rc;
    pmix_proc_t *p;
    size_t n, nmb;
    pmix_list_t plist;
    prte_namelist_t *nm;
    bool found;

    if (NULL == sig->groupID) {
        return NULL;
    }

    /* search the existing tracker list to see if this already exists - we
     * default to using the groupID if one is given, otherwise we fallback
     * to the array of participating procs */
    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.group_ops, prte_grpcomm_group_t) {
        // must match groupID's and ops
        if (0 == strcmp(sig->groupID, coll->sig->groupID) &&
            sig->op == coll->sig->op) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:group:returning existing collective %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 sig->groupID));
            // if this is a bootstrap, then we have to track the number of leaders
            if (0 < sig->bootstrap) {
                if (0 < coll->nleaders) {
                    if (coll->nleaders != sig->bootstrap) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                } else {
                    // collective tracker could have been created by a follower,
                    // which means nleaders will not have been set
                    coll->nleaders = sig->bootstrap;
                }
                coll->bootstrap = true;
                // add this proc to the list of members
                PMIX_CONSTRUCT(&plist, pmix_list_t);
                for (n=0; n < sig->nmembers; n++) {
                    // see if we already have this proc
                    found = false;
                    for (nmb=0; nmb < coll->sig->nmembers; nmb++) {
                        if (PMIX_CHECK_PROCID(&sig->members[n], &coll->sig->members[nmb])) {
                            // yes, we do
                            found = true;
                            // check for wildcard as that needs to be retained
                            if (PMIX_RANK_WILDCARD == sig->members[n].rank) {
                                coll->sig->members[n].rank = PMIX_RANK_WILDCARD;
                            }
                            break;
                        }
                    }
                    if (!found) {
                        // cache the proc
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &sig->members[n], sizeof(pmix_proc_t));
                        pmix_list_append(&plist, &nm->super);
                    }
                }
                // add any missing procs to the addmembers
                if (0 < pmix_list_get_size(&plist)) {
                    n = coll->sig->nmembers + pmix_list_get_size(&plist);
                    PMIX_PROC_CREATE(p, n);
                    if (NULL != coll->sig->members) {
                        memcpy(p, coll->sig->members, coll->sig->nmembers * sizeof(pmix_proc_t));
                    }
                    n = coll->sig->nmembers;
                    PMIX_LIST_FOREACH(nm, &plist, prte_namelist_t) {
                        memcpy(&p[n], &nm->name, sizeof(pmix_proc_t));
                        ++n;
                    }
                    PMIX_LIST_DESTRUCT(&plist);
                    if (NULL != coll->sig->members) {
                        PMIX_PROC_FREE(coll->sig->members, coll->sig->nmembers);
                    }
                    coll->sig->members = p;
                    coll->sig->nmembers = n;
                }

            } else if (sig->follower) {
                // just ensure the bootstrap flag is set
                coll->bootstrap = true;
            }

            // if we are adding members, aggregate them
            if (0 < sig->naddmembers) {
                PMIX_CONSTRUCT(&plist, pmix_list_t);
                for (n=0; n < sig->naddmembers; n++) {
                    // see if we already have this proc
                    found = false;
                    for (nmb=0; nmb < coll->sig->naddmembers; nmb++) {
                        if (PMIX_CHECK_PROCID(&sig->addmembers[n], &coll->sig->addmembers[nmb])) {
                            // yes, we do
                            found = true;
                            // check for wildcard as that needs to be retained
                            if (PMIX_RANK_WILDCARD == sig->addmembers[n].rank) {
                                coll->sig->addmembers[n].rank = PMIX_RANK_WILDCARD;
                            }
                            break;
                        }
                    }
                    if (!found) {
                        // cache the proc
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &sig->addmembers[n], sizeof(pmix_proc_t));
                        pmix_list_append(&plist, &nm->super);
                    }
                }
                // add any missing procs to the addmembers
                if (0 < pmix_list_get_size(&plist)) {
                    n = coll->sig->naddmembers + pmix_list_get_size(&plist);
                    PMIX_PROC_CREATE(p, n);
                    if (NULL != coll->sig->addmembers) {
                        memcpy(p, coll->sig->addmembers, coll->sig->naddmembers * sizeof(pmix_proc_t));
                    }
                    n = coll->sig->naddmembers;
                    PMIX_LIST_FOREACH(nm, &plist, prte_namelist_t) {
                        memcpy(&p[n], &nm->name, sizeof(pmix_proc_t));
                        ++n;
                    }
                    PMIX_LIST_DESTRUCT(&plist);
                    if (NULL != coll->sig->addmembers) {
                        PMIX_PROC_FREE(coll->sig->addmembers, coll->sig->naddmembers);
                    }
                    coll->sig->addmembers = p;
                    coll->sig->naddmembers = n;
                    coll->nfollowers = n;
                }
            }
            // if they specified a final order, see if one was already given
            if (NULL != sig->final_order) {
                if (NULL == coll->sig->final_order) {
                    // cache the directive
                    PMIX_PROC_CREATE(coll->sig->final_order, sig->nfinal);
                    memcpy(coll->sig->final_order, sig->final_order, sig->nfinal * sizeof(pmix_proc_t));
                    coll->sig->nfinal = sig->nfinal;
                } else {
                    // see if they match - for now, do a direct match
                    if (coll->sig->nfinal != sig->nfinal) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                    if (0 != memcmp(coll->sig->final_order, sig->final_order, sig->nfinal * sizeof(pmix_proc_t))) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                    // they are the same, so just ignore the new directive
                }
            }
            if (!coll->sig->assignID && sig->assignID) {
                coll->sig->assignID = true;
            }
            return coll;
        }
    }

    /* if we get here, then this is a new collective - so create
     * the tracker for it unless directed otherwise */
    if (!create) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

        return NULL;
    }

    coll = PMIX_NEW(prte_grpcomm_group_t);
    coll->sig = PMIX_NEW(prte_grpcomm_direct_group_signature_t);
    coll->sig->op = sig->op;
    coll->sig->groupID = strdup(sig->groupID);
    coll->sig->assignID = sig->assignID;
    // save the participating procs
    coll->sig->nmembers = sig->nmembers;
    if (0 < sig->nmembers) {
        coll->sig->members = (pmix_proc_t *) malloc(coll->sig->nmembers * sizeof(pmix_proc_t));
        memcpy(coll->sig->members, sig->members, coll->sig->nmembers * sizeof(pmix_proc_t));
    }
    coll->sig->naddmembers = sig->naddmembers;
    if (0 < sig->naddmembers) {
        coll->sig->addmembers = (pmix_proc_t *) malloc(coll->sig->naddmembers * sizeof(pmix_proc_t));
        memcpy(coll->sig->addmembers, sig->addmembers, coll->sig->naddmembers * sizeof(pmix_proc_t));
    }
    coll->nfollowers = coll->sig->naddmembers;
    // need to know the bootstrap in case one is ongoing
    coll->nleaders = coll->sig->bootstrap;
    if (0 < sig->bootstrap || sig->follower) {
        coll->bootstrap = true;
    }
    pmix_list_append(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);

    /* if this is a bootstrap operation, then there is no "rollup"
     * collective - each daemon reports directly to the DVM controller */
    if (coll->bootstrap) {
        return coll;
    }

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        pmix_list_remove_item(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);
        PMIX_RELEASE(coll);
        return NULL;
    }

    /* count the number of contributions we should get */
    coll->nexpected = prte_rml_get_num_contributors(coll->dmns, coll->ndmns);

    /* see if I am in the array of participants - note that I may
     * be in the rollup tree even though I'm not participating
     * in the collective itself */
    for (n = 0; n < coll->ndmns; n++) {
        if (coll->dmns[n] == PRTE_PROC_MY_NAME->rank) {
            coll->nexpected++;
            break;
        }
    }

    return coll;
}

static int create_dmns(prte_grpcomm_direct_group_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns)
{
    size_t n;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_node_t *node;
    prte_job_map_t *map;
    int i;
    pmix_list_t ds;
    prte_namelist_t *nm;
    pmix_rank_t vpid;
    bool found;
    size_t nds = 0;
    pmix_rank_t *dns = NULL;
    int rc = PRTE_SUCCESS;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:group:create_dmns called with %s signature size %" PRIsize_t "",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == sig->members) ? "NULL" : "NON-NULL", sig->nmembers));

    PMIX_CONSTRUCT(&ds, pmix_list_t);
    for (n = 0; n < sig->nmembers; n++) {
        if (NULL == (jdata = prte_get_job_data_object(sig->members[n].nspace))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        map = (prte_job_map_t*)jdata->map;
        if (NULL == map || 0 == map->num_nodes) {
            /* we haven't generated a job map yet - if we are the HNP,
             * then we should only involve ourselves. Otherwise, we have
             * no choice but to abort to avoid hangs */
            if (PRTE_PROC_IS_MASTER) {
                rc = PRTE_SUCCESS;
                break;
            }
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        if (PMIX_RANK_WILDCARD == sig->members[n].rank) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:group::create_dmns called for all procs in job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(sig->members[0].nspace)));
            /* all daemons hosting this jobid are participating */
            for (i = 0; i < map->nodes->size; i++) {
                if (NULL == (node = pmix_pointer_array_get_item(map->nodes, i))) {
                    continue;
                }
                if (NULL == node->daemon) {
                    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                    rc = PRTE_ERR_NOT_FOUND;
                    goto done;
                }
                found = false;
                PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
                {
                    if (nm->name.rank == node->daemon->name.rank) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                         "%s grpcomm:direct:group::create_dmns adding daemon %s to list",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, node->daemon->name.rank);
                    pmix_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s sign: GETTING PROC OBJECT FOR %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&sig->members[n])));
            proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs,
                                                               sig->members[n].rank);
            if (NULL == proc) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            if (NULL == proc->node || NULL == proc->node->daemon) {
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            vpid = proc->node->daemon->name.rank;
            found = false;
            PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
            {
                if (nm->name.rank == vpid) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nm = PMIX_NEW(prte_namelist_t);
                PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, vpid);
                pmix_list_append(&ds, &nm->super);
            }
        }
    }

done:
    if (0 < pmix_list_get_size(&ds)) {
        dns = (pmix_rank_t *) malloc(pmix_list_get_size(&ds) * sizeof(pmix_rank_t));
        nds = 0;
        while (NULL != (nm = (prte_namelist_t *) pmix_list_remove_first(&ds))) {
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:group::create_dmns adding daemon %s to array",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name)));
            dns[nds++] = nm->name.rank;
            PMIX_RELEASE(nm);
        }
    }
    PMIX_LIST_DESTRUCT(&ds);
    *dmns = dns;
    *ndmns = nds;
    return rc;
}

static int pack_signature(pmix_data_buffer_t *bkt,
                          prte_grpcomm_direct_group_signature_t *sig)
{
    pmix_status_t rc;

    // pack the operation
    rc = PMIx_Data_pack(NULL, bkt, &sig->op, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // add the groupID
    rc = PMIx_Data_pack(NULL, bkt, &sig->groupID, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack the flag to assign context ID
    rc = PMIx_Data_pack(NULL, bkt, &sig->assignID, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack the context ID, if one was given
    rc = PMIx_Data_pack(NULL, bkt, &sig->ctxid_assigned, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (sig->ctxid_assigned) {
        rc = PMIx_Data_pack(NULL, bkt, &sig->ctxid, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    // pack members, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->nmembers, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->nmembers) {
        rc = PMIx_Data_pack(NULL, bkt, sig->members, sig->nmembers, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    // pack bootstrap number
    rc = PMIx_Data_pack(NULL, bkt, &sig->bootstrap, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack follower flag
    rc = PMIx_Data_pack(NULL, bkt, &sig->follower, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack added membership, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->naddmembers, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->naddmembers) {
        rc = PMIx_Data_pack(NULL, bkt, sig->addmembers, sig->naddmembers, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    // pack final order, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->nfinal, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->nfinal) {
        rc = PMIx_Data_pack(NULL, bkt, sig->final_order, sig->nfinal, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    return PRTE_SUCCESS;
}

static int unpack_signature(pmix_data_buffer_t *buffer,
                            prte_grpcomm_direct_group_signature_t **sig)
{
    pmix_status_t rc;
    int32_t cnt;
    prte_grpcomm_direct_group_signature_t *s;

    s = PMIX_NEW(prte_grpcomm_direct_group_signature_t);

    // unpack the op
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->op, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the groupID
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->groupID, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack whether or not to assign a context ID
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->assignID, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the context ID, if one was assigned
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->ctxid_assigned, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (s->ctxid_assigned) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &s->ctxid, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the membership
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->nmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->nmembers) {
        PMIX_PROC_CREATE(s->members, s->nmembers);
        cnt = s->nmembers;
        rc = PMIx_Data_unpack(NULL, buffer, s->members, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the bootstrap count
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->bootstrap, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the follower flag
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->follower, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the added members
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->naddmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->naddmembers) {
        PMIX_PROC_CREATE(s->addmembers, s->naddmembers);
        cnt = s->naddmembers;
        rc = PMIx_Data_unpack(NULL, buffer, s->addmembers, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the final order
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->nfinal, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->nfinal) {
        PMIX_PROC_CREATE(s->final_order, s->nfinal);
        cnt = s->nfinal;
        rc = PMIx_Data_unpack(NULL, buffer, s->final_order, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    *sig = s;
    return PRTE_SUCCESS;
}
