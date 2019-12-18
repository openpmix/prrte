#include "prrte_config.h"

#include <stdio.h>
#include <signal.h>
#include <math.h>

#include "src/runtime/opal_progress.h"

#include "src/util/proc_info.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/rml/rml.h"
#include "src/mca/errmgr/errmgr.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_wait.h"
#include "src/mca/qos/qos.h"
#include "src/util/attr.h"

#define MY_TAG 12345
#define MAX_COUNT 3

static volatile bool msgs_recvd;
static volatile bool channel_inactive = false;
static volatile bool channel_active = false;
static volatile bool msg_active = false;
static volatile prrte_rml_channel_num_t channel;
static volatile int num_msgs_recvd = 0;
static volatile int num_msgs_sent = 0;

static void close_channel_callback(int status,
                                  prrte_rml_channel_num_t channel_num,
                                  prrte_process_name_t * peer,
                                  opal_list_t *qos_attributes,
                                  void * cbdata)
{
    if (PRRTE_SUCCESS != status)
        opal_output(0, "close channel not successful status =%d", status);
    else
        opal_output(0, "close channel successful - channel num = %d", channel_num);
    channel_active = false;
}

static void open_channel_callback(int status,
                                  prrte_rml_channel_num_t channel_num,
                                  prrte_process_name_t * peer,
                                  opal_list_t *qos_attributes,
                                  void * cbdata)
{
    if (PRRTE_SUCCESS != status) {
        opal_output(0, "open channel not successful status =%d", status);

    } else {
        channel = channel_num;
        opal_output(0, "Open channel successful - channel num = %d", channel_num);

    }
    channel_inactive = false;
}

static void send_callback(int status, prrte_process_name_t *peer,
                          opal_buffer_t* buffer, prrte_rml_tag_t tag,
                          void* cbdata)

{
    PRRTE_RELEASE(buffer);
    num_msgs_sent++;
    if (PRRTE_SUCCESS != status) {
        opal_output(0, "rml_send_nb  not successful status =%d", status);
    }
    if(num_msgs_sent == 5)
        msg_active = false;
}

static void recv_callback(int status, prrte_process_name_t *sender,
                          opal_buffer_t* buffer, prrte_rml_tag_t tag,
                          void* cbdata)

{
    //prrte_rml_recv_cb_t *blob = (prrte_rml_recv_cb_t*)cbdata;
    num_msgs_recvd++;
    opal_output(0, "recv_callback received msg =%d", num_msgs_recvd);
    if ( num_msgs_recvd == 5) {
        num_msgs_recvd =0;
        msgs_recvd = false;

    }

}

static void channel_send_callback (int status, prrte_rml_channel_num_t channel,
                                   opal_buffer_t * buffer, prrte_rml_tag_t tag,
                                   void *cbdata)
{
    PRRTE_RELEASE(buffer);
    if (PRRTE_SUCCESS != status) {
        opal_output(0, "send_nb_channel not successful status =%d", status);
    }
    msg_active = false;
}


int main(int argc, char *argv[]){
    int count;
    int msgsize;
    int *type, type_val;
    int *i, j, rc, n;
    prrte_process_name_t peer;
    double maxpower;
    opal_buffer_t *buf;
    prrte_rml_recv_cb_t blob;
    opal_list_t *qos_attributes;
    int  window;
    uint32_t timeout = 1;
    bool retry = false;
    uint8_t *msg;
    /*
     * Init
     */
    prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI);

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
    type_val = prrte_qos_ack;
    type = &type_val;
    window = 5;
    count =3;
    qos_attributes = PRRTE_NEW (opal_list_t);
    if (PRRTE_SUCCESS == (rc = prrte_set_attribute( qos_attributes,
                                  PRRTE_QOS_TYPE, PRRTE_ATTR_GLOBAL, (void*)type, PRRTE_UINT8))) {
        type = &window;
        if (PRRTE_SUCCESS == (rc = prrte_set_attribute(qos_attributes, PRRTE_QOS_WINDOW_SIZE,
                                      PRRTE_ATTR_GLOBAL, (void*) type, PRRTE_UINT32))) {
            //  prrte_get_attribute( &qos_attributes, PRRTE_QOS_WINDOW_SIZE, (void**)&type, PRRTE_UINT32);
            // opal_output(0, "%s set attribute window =%d complete \n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), *type );
            type = &timeout;
            prrte_set_attribute (qos_attributes, PRRTE_QOS_ACK_NACK_TIMEOUT, PRRTE_ATTR_GLOBAL,
                                    (void*)type, PRRTE_UINT32);
            prrte_set_attribute (qos_attributes, PRRTE_QOS_MSG_RETRY, PRRTE_ATTR_GLOBAL,
                                    NULL, PRRTE_BOOL);
           /* Uncomment following lines to print channel attributes */
           /*
           opal_output(0, "%s set attribute retry =%d complete \n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), retry );
           prrte_get_attribute( qos_attributes, PRRTE_QOS_TYPE, (void**)&type, PRRTE_UINT8);
           opal_output(0, "%s set attribute type =%d complete \n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), *type );
           prrte_get_attribute( qos_attributes, PRRTE_QOS_WINDOW_SIZE, (void**)&type, PRRTE_UINT32);
           opal_output(0, "%s set attribute window =%d complete \n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), *type )
           prrte_get_attribute( qos_attributes, PRRTE_QOS_ACK_NACK_TIMEOUT, (void**)&type, PRRTE_UINT32);
           opal_output(0, "%s set attribute timeout =%d complete \n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), *type );*/
           channel_inactive = true;
           prrte_rml.open_channel ( &peer, qos_attributes, open_channel_callback, NULL);
           opal_output(0, "%s process sent open channel request %d waiting for completion \n",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
           PRRTE_WAIT_FOR_COMPLETION(channel_inactive);
           opal_output(0, "%s open channel complete to %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                                                 PRRTE_NAME_PRINT(&peer));
          }
    }
    for (j = 0; j< count; j++)
    {
        if (PRRTE_PROC_MY_NAME->vpid == 0)
        {
            /* rank0 starts ring */
            msg_active = true;
            for (n = 0; n< window; n++ )
            {
                buf = PRRTE_NEW(opal_buffer_t);
                maxpower = (double)(j%7);
                msgsize = (int)pow(10.0, maxpower);
                opal_output(0, "Ring %d message %d size %d bytes", j,n, msgsize);
                msg = (uint8_t*)malloc(msgsize);
                opal_dss.pack(buf, msg, msgsize, PRRTE_BYTE);
                free(msg);
                prrte_rml.send_buffer_channel_nb(channel, buf, MY_TAG, channel_send_callback, NULL);
                PRRTE_CONSTRUCT(&blob, prrte_rml_recv_cb_t);
                blob.active = true;
                prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, MY_TAG,
                                        PRRTE_RML_NON_PERSISTENT,
                                        prrte_rml_recv_callback, &blob);
                PRRTE_WAIT_FOR_COMPLETION(blob.active);
                PRRTE_DESTRUCT(&blob);
                //prrte_rml.send_buffer_nb(&peer, buf,MY_TAG, send_callback, NULL)
            }
            PRRTE_WAIT_FOR_COMPLETION(msg_active);
            opal_output(0, "%s Ring %d completed", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
            //sleep(2);
        }
        else
        {
            msg_active = true;
            for (n =0; n < window; n++) {
                PRRTE_CONSTRUCT(&blob, prrte_rml_recv_cb_t);
                blob.active = true;
                prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD, MY_TAG,
                                PRRTE_RML_NON_PERSISTENT,
                                prrte_rml_recv_callback, &blob);
                PRRTE_WAIT_FOR_COMPLETION(blob.active);
                opal_output(0, "%s received message %d from %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j,
                                PRRTE_NAME_PRINT(&blob.name));
                /* send it along */
               buf = PRRTE_NEW(opal_buffer_t);
               opal_dss.copy_payload(buf, &blob.data);
               PRRTE_DESTRUCT(&blob);
               prrte_rml.send_buffer_channel_nb(channel, buf, MY_TAG, channel_send_callback, NULL);
            }
            PRRTE_WAIT_FOR_COMPLETION(msg_active);
            opal_output(0, "%s Ring %d completed", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), j);
            //sleep (2);
        }
    }
    channel_active = true;
    prrte_rml.close_channel ( channel,close_channel_callback, NULL);
    opal_output(0, "%s process sent close channel request waiting for completion \n",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
    PRRTE_WAIT_FOR_COMPLETION(channel_active);
    opal_output(0, "%s close channel complete to %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                PRRTE_NAME_PRINT(&peer));
    prrte_finalize();
    return 0;
}
