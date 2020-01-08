/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_ATTRS_H
#define PRRTE_ATTRS_H

#include "prrte_config.h"
#include "types.h"

/*** FLAG FOR SETTING ATTRIBUTES - INDICATES IF THE
 *** ATTRIBUTE IS TO BE SHARED WITH REMOTE PROCS OR NOT
 */
#define PRRTE_ATTR_LOCAL    true      // for local use only
#define PRRTE_ATTR_GLOBAL   false     // include when sending this object

/* define the mininum value of the PRRTE keys just in
 * case someone someday puts a layer underneath us */
#define PRRTE_ATTR_KEY_BASE        0

/*** ATTRIBUTE FLAGS - never sent anywwhere ***/
typedef uint8_t prrte_app_context_flags_t;
#define PRRTE_APP_FLAG_USED_ON_NODE  0x01  // is being used on the local node


/* APP_CONTEXT ATTRIBUTE KEYS */
#define PRRTE_APP_HOSTFILE            1    // string  - hostfile
#define PRRTE_APP_ADD_HOSTFILE        2    // string  - hostfile to be added
#define PRRTE_APP_DASH_HOST           3    // string  - hosts specified with -host option
#define PRRTE_APP_ADD_HOST            4    // string  - hosts to be added
#define PRRTE_APP_USER_CWD            5    // bool  - user specified cwd
#define PRRTE_APP_SSNDIR_CWD          6    // bool  - use session dir as cwd
#define PRRTE_APP_PRELOAD_BIN         7    // bool  - move binaries to remote nodes prior to exec
#define PRRTE_APP_PRELOAD_FILES       8    // string  - files to be moved to remote nodes prior to exec
#define PRRTE_APP_SSTORE_LOAD         9    // string
#define PRRTE_APP_RECOV_DEF          10    // bool  - whether or not a recovery policy was defined
#define PRRTE_APP_MAX_RESTARTS       11    // int32 - max number of times a process can be restarted
#define PRRTE_APP_MIN_NODES          12    // int64 - min number of nodes required
#define PRRTE_APP_MANDATORY          13    // bool - flag if nodes requested in -host are "mandatory" vs "optional"
#define PRRTE_APP_MAX_PPN            14    // uint32 - maximum number of procs/node for this app
#define PRRTE_APP_PREFIX_DIR         15    // string - prefix directory for this app, if override necessary
#define PRRTE_APP_NO_CACHEDIR        16    // bool - flag that a cache dir is not to be specified for a Singularity container
#define PRRTE_APP_SET_ENVAR          17    // prrte_envar_t - set the given envar to the specified value
#define PRRTE_APP_UNSET_ENVAR        18    // string - name of envar to unset, if present
#define PRRTE_APP_PREPEND_ENVAR      19    // prrte_envar_t - prepend the specified value to the given envar
#define PRRTE_APP_APPEND_ENVAR       20    // prrte_envar_t - append the specified value to the given envar
#define PRRTE_APP_ADD_ENVAR          21    // prrte_envar_t - add envar, do not override pre-existing one
#define PRRTE_APP_DEBUGGER_DAEMON    22    // bool - flag that this app describes daemons to be co-launched
                                          //        with the application procs in the other apps
#define PRRTE_APP_PSET_NAME          23    // string - user-assigned name for the process
                                          //          set containing the given process

#define PRRTE_APP_MAX_KEY        100


/*** NODE FLAGS - never sent anywhere ***/
typedef uint8_t prrte_node_flags_t;
#define PRRTE_NODE_FLAG_DAEMON_LAUNCHED    0x01   // whether or not the daemon on this node has been launched
#define PRRTE_NODE_FLAG_LOC_VERIFIED       0x02   // whether or not the location has been verified - used for
                                                 // environments where the daemon's final destination is uncertain
#define PRRTE_NODE_FLAG_OVERSUBSCRIBED     0x04   // whether or not this node is oversubscribed
#define PRRTE_NODE_FLAG_MAPPED             0x08   // whether we have been added to the current map
#define PRRTE_NODE_FLAG_SLOTS_GIVEN        0x10   // the number of slots was specified - used only in non-managed environments
#define PRRTE_NODE_NON_USABLE              0x20   // the node is hosting a tool and is NOT to be used for jobs


/*** NODE ATTRIBUTE KEYS - never sent anywhere ***/
#define PRRTE_NODE_START_KEY    PRRTE_APP_MAX_KEY

#define PRRTE_NODE_USERNAME       (PRRTE_NODE_START_KEY + 1)
#define PRRTE_NODE_LAUNCH_ID      (PRRTE_NODE_START_KEY + 2)   // int32 - Launch id needed by some systems to launch a proc on this node
#define PRRTE_NODE_HOSTID         (PRRTE_NODE_START_KEY + 3)   // prrte_vpid_t - if this "node" is a coprocessor being hosted on a different node, then
                                                             // we need to know the id of our "host" to help any procs on us to determine locality
#define PRRTE_NODE_ALIAS          (PRRTE_NODE_START_KEY + 4)   // comma-separate list of alternate names for the node
#define PRRTE_NODE_SERIAL_NUMBER  (PRRTE_NODE_START_KEY + 5)   // string - serial number: used if node is a coprocessor
#define PRRTE_NODE_PORT           (PRRTE_NODE_START_KEY + 6)   // int32 - Alternate port to be passed to plm

#define PRRTE_NODE_MAX_KEY        200

/*** JOB FLAGS - included in prrte_job_t transmissions ***/
typedef uint16_t prrte_job_flags_t;
#define PRRTE_JOB_FLAGS_T  PRRTE_UINT16
#define PRRTE_JOB_FLAG_UPDATED            0x0001   // job has been updated and needs to be included in the pidmap message
#define PRRTE_JOB_FLAG_RESTARTED          0x0004   // some procs in this job are being restarted
#define PRRTE_JOB_FLAG_ABORTED            0x0008   // did this job abort?
#define PRRTE_JOB_FLAG_DEBUGGER_DAEMON    0x0010   // job is launching debugger daemons
#define PRRTE_JOB_FLAG_FORWARD_OUTPUT     0x0020   // forward output from the apps
#define PRRTE_JOB_FLAG_DO_NOT_MONITOR     0x0040   // do not monitor apps for termination
#define PRRTE_JOB_FLAG_FORWARD_COMM       0x0080   //
#define PRRTE_JOB_FLAG_RECOVERABLE        0x0100   // job is recoverable
#define PRRTE_JOB_FLAG_RESTART            0x0200   //
#define PRRTE_JOB_FLAG_PROCS_MIGRATING    0x0400   // some procs in job are migrating from one node to another
#define PRRTE_JOB_FLAG_OVERSUBSCRIBED     0x0800   // at least one node in the job is oversubscribed
#define PRRTE_JOB_FLAG_TOOL               0x1000   // job is a tool
#define PRRTE_JOB_FLAG_LAUNCHER           0x2000   // job is also a launcher

/***   JOB ATTRIBUTE KEYS   ***/
#define PRRTE_JOB_START_KEY   PRRTE_NODE_MAX_KEY

#define PRRTE_JOB_LAUNCH_MSG_SENT        (PRRTE_JOB_START_KEY + 1)     // timeval - time launch message was sent
#define PRRTE_JOB_LAUNCH_MSG_RECVD       (PRRTE_JOB_START_KEY + 2)     // timeval - time launch message was recvd
#define PRRTE_JOB_MAX_LAUNCH_MSG_RECVD   (PRRTE_JOB_START_KEY + 3)     // timeval - max time for launch msg to be received
#define PRRTE_JOB_CKPT_STATE             (PRRTE_JOB_START_KEY + 5)     // size_t - ckpt state
#define PRRTE_JOB_SNAPSHOT_REF           (PRRTE_JOB_START_KEY + 6)     // string - snapshot reference
#define PRRTE_JOB_SNAPSHOT_LOC           (PRRTE_JOB_START_KEY + 7)     // string - snapshot location
#define PRRTE_JOB_SNAPC_INIT_BAR         (PRRTE_JOB_START_KEY + 8)     // prrte_grpcomm_coll_id_t - collective id
#define PRRTE_JOB_SNAPC_FINI_BAR         (PRRTE_JOB_START_KEY + 9)     // prrte_grpcomm_coll_id_t - collective id
#define PRRTE_JOB_NUM_NONZERO_EXIT       (PRRTE_JOB_START_KEY + 10)    // int32 - number of procs with non-zero exit codes
#define PRRTE_JOB_FAILURE_TIMER_EVENT    (PRRTE_JOB_START_KEY + 11)    // prrte_ptr (prrte_timer_t*) - timer event for failure detect/response if fails to launch
#define PRRTE_JOB_ABORTED_PROC           (PRRTE_JOB_START_KEY + 12)    // prrte_ptr (prrte_proc_t*) - proc that caused abort to happen
#define PRRTE_JOB_MAPPER                 (PRRTE_JOB_START_KEY + 13)    // bool - job consists of MapReduce mappers
#define PRRTE_JOB_REDUCER                (PRRTE_JOB_START_KEY + 14)    // bool - job consists of MapReduce reducers
#define PRRTE_JOB_COMBINER               (PRRTE_JOB_START_KEY + 15)    // bool - job consists of MapReduce combiners
#define PRRTE_JOB_INDEX_ARGV             (PRRTE_JOB_START_KEY + 16)    // bool - automatically index argvs
#define PRRTE_JOB_NO_VM                  (PRRTE_JOB_START_KEY + 17)    // bool - do not use VM launch
#define PRRTE_JOB_SPIN_FOR_DEBUG         (PRRTE_JOB_START_KEY + 18)    // bool - job consists of continuously operating apps
#define PRRTE_JOB_CONTINUOUS_OP          (PRRTE_JOB_START_KEY + 19)    // bool - recovery policy defined for job
#define PRRTE_JOB_RECOVER_DEFINED        (PRRTE_JOB_START_KEY + 20)    // bool - recovery policy has been defined
#define PRRTE_JOB_NON_PRRTE_JOB           (PRRTE_JOB_START_KEY + 22)    // bool - non-prrte job
#define PRRTE_JOB_STDOUT_TARGET          (PRRTE_JOB_START_KEY + 23)    // prrte_jobid_t - job that is to receive the stdout (on its stdin) from this one
#define PRRTE_JOB_POWER                  (PRRTE_JOB_START_KEY + 24)    // string - power setting for nodes in job
#define PRRTE_JOB_MAX_FREQ               (PRRTE_JOB_START_KEY + 25)    // string - max freq setting for nodes in job
#define PRRTE_JOB_MIN_FREQ               (PRRTE_JOB_START_KEY + 26)    // string - min freq setting for nodes in job
#define PRRTE_JOB_GOVERNOR               (PRRTE_JOB_START_KEY + 27)    // string - governor used for nodes in job
#define PRRTE_JOB_FAIL_NOTIFIED          (PRRTE_JOB_START_KEY + 28)    // bool - abnormal term of proc within job has been reported
#define PRRTE_JOB_TERM_NOTIFIED          (PRRTE_JOB_START_KEY + 29)    // bool - normal term of job has been reported
#define PRRTE_JOB_PEER_MODX_ID           (PRRTE_JOB_START_KEY + 30)    // prrte_grpcomm_coll_id_t - collective id
#define PRRTE_JOB_INIT_BAR_ID            (PRRTE_JOB_START_KEY + 31)    // prrte_grpcomm_coll_id_t - collective id
#define PRRTE_JOB_FINI_BAR_ID            (PRRTE_JOB_START_KEY + 32)    // prrte_grpcomm_coll_id_t - collective id
#define PRRTE_JOB_FWDIO_TO_TOOL          (PRRTE_JOB_START_KEY + 33)    // Forward IO for this job to the tool requesting its spawn
#define PRRTE_JOB_PHYSICAL_CPUIDS        (PRRTE_JOB_START_KEY + 34)    // bool - Hostfile contains physical jobids in cpuset
#define PRRTE_JOB_LAUNCHED_DAEMONS       (PRRTE_JOB_START_KEY + 35)    // bool - Job caused new daemons to be spawned
#define PRRTE_JOB_REPORT_BINDINGS        (PRRTE_JOB_START_KEY + 36)    // bool - Report process bindings
#define PRRTE_JOB_CPU_LIST               (PRRTE_JOB_START_KEY + 37)    // string - cpus to which procs are to be bound
#define PRRTE_JOB_NOTIFICATIONS          (PRRTE_JOB_START_KEY + 38)    // string - comma-separated list of desired notifications+methods
#define PRRTE_JOB_ROOM_NUM               (PRRTE_JOB_START_KEY + 39)    // int - number of remote request's hotel room
#define PRRTE_JOB_LAUNCH_PROXY           (PRRTE_JOB_START_KEY + 40)    // prrte_process_name_t - name of spawn requestor
#define PRRTE_JOB_NSPACE_REGISTERED      (PRRTE_JOB_START_KEY + 41)    // bool - job has been registered with embedded PMIx server
#define PRRTE_JOB_FIXED_DVM              (PRRTE_JOB_START_KEY + 42)    // bool - do not change the size of the DVM for this job
#define PRRTE_JOB_DVM_JOB                (PRRTE_JOB_START_KEY + 43)    // bool - job is using a DVM
#define PRRTE_JOB_CANCELLED              (PRRTE_JOB_START_KEY + 44)    // bool - job was cancelled
#define PRRTE_JOB_OUTPUT_TO_FILE         (PRRTE_JOB_START_KEY + 45)    // string - path to use as basename of files to which stdout/err is to be directed
#define PRRTE_JOB_MERGE_STDERR_STDOUT    (PRRTE_JOB_START_KEY + 46)    // bool - merge stderr into stdout stream
#define PRRTE_JOB_TAG_OUTPUT             (PRRTE_JOB_START_KEY + 47)    // bool - tag stdout/stderr
#define PRRTE_JOB_TIMESTAMP_OUTPUT       (PRRTE_JOB_START_KEY + 48)    // bool - timestamp stdout/stderr
#define PRRTE_JOB_MULTI_DAEMON_SIM       (PRRTE_JOB_START_KEY + 49)    // bool - multiple daemons/node to simulate large cluster
#define PRRTE_JOB_NOTIFY_COMPLETION      (PRRTE_JOB_START_KEY + 50)    // bool - notify parent proc when spawned job terminates
#define PRRTE_JOB_TRANSPORT_KEY          (PRRTE_JOB_START_KEY + 51)    // string - transport keys assigned to this job
#define PRRTE_JOB_INFO_CACHE             (PRRTE_JOB_START_KEY + 52)    // prrte_list_t - list of prrte_value_t to be included in job_info
#define PRRTE_JOB_FULLY_DESCRIBED        (PRRTE_JOB_START_KEY + 53)    // bool - job is fully described in launch msg
#define PRRTE_JOB_SILENT_TERMINATION     (PRRTE_JOB_START_KEY + 54)    // bool - do not generate an event notification when job
                                                                     //        normally terminates
#define PRRTE_JOB_SET_ENVAR              (PRRTE_JOB_START_KEY + 55)    // prrte_envar_t - set the given envar to the specified value
#define PRRTE_JOB_UNSET_ENVAR            (PRRTE_JOB_START_KEY + 56)    // string - name of envar to unset, if present
#define PRRTE_JOB_PREPEND_ENVAR          (PRRTE_JOB_START_KEY + 57)    // prrte_envar_t - prepend the specified value to the given envar
#define PRRTE_JOB_APPEND_ENVAR           (PRRTE_JOB_START_KEY + 58)    // prrte_envar_t - append the specified value to the given envar
#define PRRTE_JOB_ADD_ENVAR              (PRRTE_JOB_START_KEY + 59)    // prrte_envar_t - add envar, do not override pre-existing one
#define PRRTE_JOB_APP_SETUP_DATA         (PRRTE_JOB_START_KEY + 60)    // prrte_byte_object_t - blob containing app setup data
#define PRRTE_JOB_OUTPUT_TO_DIRECTORY    (PRRTE_JOB_START_KEY + 61)    // string - path of directory to which stdout/err is to be directed

#define PRRTE_JOB_MAX_KEY   300


/*** PROC FLAGS - never sent anywhere ***/
typedef uint16_t prrte_proc_flags_t;
#define PRRTE_PROC_FLAG_ALIVE         0x0001  // proc has been launched and has not yet terminated
#define PRRTE_PROC_FLAG_ABORT         0x0002  // proc called abort
#define PRRTE_PROC_FLAG_UPDATED       0x0004  // proc has been updated and need to be included in the next pidmap message
#define PRRTE_PROC_FLAG_LOCAL         0x0008  // indicate that this proc is local
#define PRRTE_PROC_FLAG_REPORTED      0x0010  // indicate proc has reported in
#define PRRTE_PROC_FLAG_REG           0x0020  // proc has registered
#define PRRTE_PROC_FLAG_HAS_DEREG     0x0040  // proc has deregistered
#define PRRTE_PROC_FLAG_AS_MPI        0x0080  // proc is MPI process
#define PRRTE_PROC_FLAG_IOF_COMPLETE  0x0100  // IOF has completed
#define PRRTE_PROC_FLAG_WAITPID       0x0200  // waitpid fired
#define PRRTE_PROC_FLAG_RECORDED      0x0400  // termination has been recorded
#define PRRTE_PROC_FLAG_DATA_IN_SM    0x0800  // modex data has been stored in the local shared memory region
#define PRRTE_PROC_FLAG_DATA_RECVD    0x1000  // modex data for this proc has been received
#define PRRTE_PROC_FLAG_SM_ACCESS     0x2000  // indicate if process can read modex data from shared memory region
#define PRRTE_PROC_FLAG_TOOL          0x4000  // proc is a tool and doesn't count against allocations

/***   PROCESS ATTRIBUTE KEYS   ***/
#define PRRTE_PROC_START_KEY   PRRTE_JOB_MAX_KEY

#define PRRTE_PROC_NOBARRIER       (PRRTE_PROC_START_KEY +  1)           // bool  - indicates proc should not barrier in prrte_init
#define PRRTE_PROC_CPU_BITMAP      (PRRTE_PROC_START_KEY +  2)           // string - string representation of cpu bindings
#define PRRTE_PROC_HWLOC_LOCALE    (PRRTE_PROC_START_KEY +  3)           // prrte_ptr (hwloc_obj_t) = pointer to object where proc was mapped
#define PRRTE_PROC_HWLOC_BOUND     (PRRTE_PROC_START_KEY +  4)           // prrte_ptr (hwloc_obj_t) = pointer to object where proc was bound
#define PRRTE_PROC_PRIOR_NODE      (PRRTE_PROC_START_KEY +  5)           // void* - pointer to prrte_node_t where this proc last executed
#define PRRTE_PROC_NRESTARTS       (PRRTE_PROC_START_KEY +  6)           // int32 - number of times this process has been restarted
#define PRRTE_PROC_RESTART_TIME    (PRRTE_PROC_START_KEY +  7)           // timeval - time of last restart
#define PRRTE_PROC_FAST_FAILS      (PRRTE_PROC_START_KEY +  8)           // int32 - number of failures in "fast" window
#define PRRTE_PROC_CKPT_STATE      (PRRTE_PROC_START_KEY +  9)           // size_t - ckpt state
#define PRRTE_PROC_SNAPSHOT_REF    (PRRTE_PROC_START_KEY + 10)           // string - snapshot reference
#define PRRTE_PROC_SNAPSHOT_LOC    (PRRTE_PROC_START_KEY + 11)           // string - snapshot location
#define PRRTE_PROC_NODENAME        (PRRTE_PROC_START_KEY + 12)           // string - node where proc is located, used only by tools
#define PRRTE_PROC_CGROUP          (PRRTE_PROC_START_KEY + 13)           // string - name of cgroup this proc shall be assigned to
#define PRRTE_PROC_NBEATS          (PRRTE_PROC_START_KEY + 14)           // int32 - number of heartbeats in current window

#define PRRTE_PROC_MAX_KEY   400

/*** RML ATTRIBUTE keys ***/
#define PRRTE_RML_START_KEY  PRRTE_PROC_MAX_KEY
#define PRRTE_RML_TRANSPORT_TYPE         (PRRTE_RML_START_KEY +  1)   // string - null terminated string containing transport type
#define PRRTE_RML_PROTOCOL_TYPE          (PRRTE_RML_START_KEY +  2)   // string - protocol type (e.g., as returned by fi_info)
#define PRRTE_RML_CONDUIT_ID             (PRRTE_RML_START_KEY +  3)   // prrte_rml_conduit_t - conduit_id for this transport
#define PRRTE_RML_INCLUDE_COMP_ATTRIB    (PRRTE_RML_START_KEY +  4)   // string - comma delimited list of RML component names to be considered
#define PRRTE_RML_EXCLUDE_COMP_ATTRIB    (PRRTE_RML_START_KEY +  5)   // string - comma delimited list of RML component names to be excluded
#define PRRTE_RML_TRANSPORT_ATTRIB       (PRRTE_RML_START_KEY +  6)   // string - comma delimited list of transport types to be considered (e.g., "fabric,ethernet")
#define PRRTE_RML_QUALIFIER_ATTRIB       (PRRTE_RML_START_KEY +  7)   // string - comma delimited list of qualifiers (e.g., routed=direct,bandwidth=xxx)
#define PRRTE_RML_PROVIDER_ATTRIB        (PRRTE_RML_START_KEY +  8)   // string - comma delimited list of provider names to be considered
#define PRRTE_RML_PROTOCOL_ATTRIB        (PRRTE_RML_START_KEY +  9)   // string - comma delimited list of protocols to be considered (e.g., tcp,udp)
#define PRRTE_RML_ROUTED_ATTRIB          (PRRTE_RML_START_KEY + 10)   // string - comma delimited list of routed modules to be considered

#define PRRTE_ATTR_KEY_MAX  1000


/*** FLAG OPS ***/
#define PRRTE_FLAG_SET(p, f)         ((p)->flags |= (f))
#define PRRTE_FLAG_UNSET(p, f)       ((p)->flags &= ~(f))
#define PRRTE_FLAG_TEST(p, f)        ((p)->flags & (f))

PRRTE_EXPORT const char *prrte_attr_key_to_str(prrte_attribute_key_t key);

/* Retrieve the named attribute from a list */
PRRTE_EXPORT bool prrte_get_attribute(prrte_list_t *attributes, prrte_attribute_key_t key,
                                      void **data, prrte_data_type_t type);

/* Set the named attribute in a list, overwriting any prior entry */
PRRTE_EXPORT int prrte_set_attribute(prrte_list_t *attributes, prrte_attribute_key_t key,
                                     bool local, void *data, prrte_data_type_t type);

/* Remove the named attribute from a list */
PRRTE_EXPORT void prrte_remove_attribute(prrte_list_t *attributes, prrte_attribute_key_t key);

PRRTE_EXPORT prrte_attribute_t* prrte_fetch_attribute(prrte_list_t *attributes,
                                                     prrte_attribute_t *prev,
                                                     prrte_attribute_key_t key);

PRRTE_EXPORT int prrte_add_attribute(prrte_list_t *attributes,
                                     prrte_attribute_key_t key, bool local,
                                     void *data, prrte_data_type_t type);

PRRTE_EXPORT int prrte_prepend_attribute(prrte_list_t *attributes,
                                         prrte_attribute_key_t key, bool local,
                                         void *data, prrte_data_type_t type);

PRRTE_EXPORT int prrte_attr_load(prrte_attribute_t *kv,
                                 void *data, prrte_data_type_t type);

PRRTE_EXPORT int prrte_attr_unload(prrte_attribute_t *kv,
                                   void **data, prrte_data_type_t type);

/*
 * Register a handler for converting attr keys to strings
 *
 * Handlers will be invoked by prrte_attr_key_to_str to return the appropriate value.
 */
typedef char* (*prrte_attr2str_fn_t)(prrte_attribute_key_t key);

PRRTE_EXPORT int prrte_attr_register(const char *project,
                                     prrte_attribute_key_t key_base,
                                     prrte_attribute_key_t key_max,
                                     prrte_attr2str_fn_t converter);

#endif
