#include "prrte_config.h"

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <sys/time.h>

#include "src/runtime/opal_progress.h"

#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_wait.h"
#include "src/util/attr.h"

#define MY_TAG 12345
#define MAX_COUNT 3

static bool msg_recvd;
static volatile bool msg_active;

static void send_callback(int status, prrte_process_name_t *peer,
                          opal_buffer_t* buffer, prrte_rml_tag_t tag,
                          void* cbdata)

{
    PRRTE_RELEASE(buffer);
    if (PRRTE_SUCCESS != status) {
        exit(1);
    }
    msg_active = false;
}


int
main(int argc, char *argv[]){
    int count;
    int msgsize;
    uint8_t *msg;
    int i, j, rc;
    prrte_process_name_t peer;
    double maxpower;
    opal_buffer_t *buf;
    prrte_rml_recv_cb_t blob;
    int conduit_id = 0;  //use the first available conduit
    struct timeval start, end;
    opal_list_t *conduit_attr;


    /*
     * Init
     */
    prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI);


    conduit_attr = PRRTE_NEW(opal_list_t);
   if( PRRTE_SUCCESS ==
            ( prrte_set_attribute( conduit_attr, PRRTE_RML_PROVIDER_ATTRIB, PRRTE_ATTR_GLOBAL,"sockets",PRRTE_STRING)))   {
    if( PRRTE_SUCCESS ==
            ( prrte_set_attribute( conduit_attr, PRRTE_RML_INCLUDE_COMP_ATTRIB, PRRTE_ATTR_GLOBAL,"ofi",PRRTE_STRING)))   {
        opal_output(0, "%s calling open_conduit with PRRTE_RML_INCLUDE_COMP_ATTRIB and PRRTE_RML_OFI_PROV_NAME_ATTRIB",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        conduit_id = prrte_rml_API_open_conduit(conduit_attr);
        if (0 > conduit_id ) {
            opal_output(0, "Conduit could not be opened for OFI, exiting");
            return;
        }
     }
   }

    opal_output(0, "Using conduit-id %d ", conduit_id);

    if (argc > 1) {
        count = atoi(argv[1]);
        if (count < 0) {
            count = INT_MAX-1;
        }
    } else {
        count = MAX_COUNT;
    }

    peer.jobid = PRRTE_PROC_MY_NAME->jobid;
    peer.vpid = PRRTE_PROC_MY_NAME->vpid + 1;
    if (peer.vpid == prrte_process_info.num_procs) {
        peer.vpid = 0;
    }

    gettimeofday(&start, NULL);
    for (j=1; j < count+1; j++) {
        /* rank0 starts ring */
        if (PRRTE_PROC_MY_NAME->vpid == 0) {
            /* setup the initiating buffer - put random sized message in it */
            buf = PRRTE_NEW(opal_buffer_t);

            maxpower = (double)(j%7);
            msgsize = (int)pow(10.0, maxpower);
            opal_output(0, "Ring %d message size %d bytes", j, msgsize);
            msg = (uint8_t*)malloc(msgsize);
            opal_dss.pack(buf, msg, msgsize, PRRTE_BYTE);
            free(msg);
            prrte_rml.send_buffer_nb(conduit_id,&peer, buf, MY_TAG, prrte_rml_send_callback, NULL);

            /* wait for it to come around */
            PRRTE_CONSTRUCT(&blob, prrte_rml_recv_cb_t);
            blob.active = true;
            prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, MY_TAG,
                                    PRRTE_RML_NON_PERSISTENT,
                                    prrte_rml_recv_callback, &blob);
            PRRTE_WAIT_FOR_COMPLETION(blob.active);
            PRRTE_DESTRUCT(&blob);

            opal_output(0, "%s Ring %d completed", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
        } else {
            /* wait for msg */
            PRRTE_CONSTRUCT(&blob, prrte_rml_recv_cb_t);
            blob.active = true;
            prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, MY_TAG,
                                    PRRTE_RML_NON_PERSISTENT,
                                    prrte_rml_recv_callback, &blob);
            PRRTE_WAIT_FOR_COMPLETION(blob.active);

            opal_output(0, "%s received message %d from %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j, PRRTE_NAME_PRINT(&blob.name));

            /* send it along */
            buf = PRRTE_NEW(opal_buffer_t);
            opal_dss.copy_payload(buf, &blob.data);
            PRRTE_DESTRUCT(&blob);
            msg_active = true;
            prrte_rml.send_buffer_nb(conduit_id,&peer, buf, MY_TAG, send_callback, NULL);
            PRRTE_WAIT_FOR_COMPLETION(msg_active);
        }
    }
    gettimeofday(&end, NULL);
    prrte_finalize();
    printf("start: %d secs, %d usecs\n",start.tv_sec,start.tv_usec);
    printf("end: %d secs, %d usecs\n",end.tv_sec,end.tv_usec);
    printf("Total minutes = %d, Total seconds = %d", (end.tv_sec - start.tv_sec)/60, (end.tv_sec - start.tv_sec)   );
    return 0;
}
