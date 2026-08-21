/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_CONSTANTS_H
#define PRTE_CONSTANTS_H

#include "prte_config.h"

#include <pmix_common.h>

BEGIN_C_DECLS

/* PRRTE's return codes are negative offsets from a single base, running
 * -3001 downwards.  PRTE_SUCCESS is zero and is the only code that is not
 * an offset.  Every value must be unique: a duplicate makes two distinct
 * codes compare equal, which is a miserable bug to find.  Uniqueness is
 * enforced by the compiler -- prte_strerror() switches on every code, so a
 * repeated value is a duplicate-case error -- and test/unit/include checks
 * the range arithmetic around it.
 *
 * GOLDEN RULE: the base is PMIX_EXTERNAL_ERR_BASE, and must be.
 *
 * PMIx reserves everything below that constant for projects layered on top
 * of it -- "negative values larger than this are guaranteed not to conflict
 * with PMIx values" -- and PRRTE is such a project.  PRRTE and PMIx codes
 * cross paths constantly, and a code that escapes prte_pmix_convert_rc() /
 * prte_pmix_convert_status() does not look foreign if the two schemes
 * overlap: it looks like some other, real code.
 *
 * This is not hypothetical.  PRRTE used to base its codes at 0, and 46 of
 * them were numerically identical to a PMIx status meaning something else:
 * PRTE_ERR_SLURM_SHRINK_FAILURE was PMIX_OPERATION_SUCCEEDED,
 * PRTE_ERR_NO_PATH_TO_TARGET was PMIX_ERR_EVENT_REGISTRATION, and
 * PRTE_ERR_TAKE_NEXT_OPTION -- a control-flow signal, not a failure -- was
 * PMIX_ERR_NOT_FOUND.  Follow PMIx's own instruction and define these
 * relative to the constant, never to a specific value: PMIx may move it.
 *
 * The list below is in two groups, split at offset 100 so each has room to
 * grow: the codes PRRTE mirrors from PMIx so a notification status has a
 * PRRTE spelling (offsets 1..72), and the codes that are PRRTE's alone
 * (offsets 101..).  That is a convention for readers, not arithmetic --
 * appending at the end of either group is fine, and appending at the very
 * end always is.
 *
 * There used to be a second base, PRTE_ERR_SPLIT, for the lower group.  It
 * came from ORTE, where the two groups belonged to two separate projects --
 * OPAL owned the first and ORTE started where OPAL's allocation ended, so
 * each could append without coordinating with the other.  OPAL and ORTE
 * were merged into this one tree in 2019, and a second base for a second
 * half of one enum in one file bought nothing after that but a pair of
 * "did the bands collide" questions.  (It also arrived here mis-typed:
 * ORTE's base was OPAL_ERR_MAX == -100, and the minus sign was dropped in
 * the merge, so for several years the whole lower group was a set of
 * *positive* error codes and PRTE_ERR_MAX evaluated to 0 -- the value of
 * PRTE_SUCCESS.)
 */
#define PRTE_ERR_BASE PMIX_EXTERNAL_ERR_BASE

enum {
    /* Error codes inherited from PRTE.  Still enum values so that we
       get the nice debugger help. */

    /* Success is zero, and is the ONE code not numbered off the base -
     * every caller in the tree spells its check "PRTE_SUCCESS != rc". */
    PRTE_SUCCESS = 0,

    PRTE_ERROR = (PRTE_ERR_BASE - 1),
    PRTE_ERR_OUT_OF_RESOURCE = (PRTE_ERR_BASE - 2),      /* fatal error */
    PRTE_ERR_TEMP_OUT_OF_RESOURCE = (PRTE_ERR_BASE - 3), /* try again later */
    PRTE_ERR_RESOURCE_BUSY = (PRTE_ERR_BASE - 4),
    PRTE_ERR_BAD_PARAM = (PRTE_ERR_BASE - 5), /* equivalent to MPI_ERR_ARG error code */
    PRTE_ERR_FATAL = (PRTE_ERR_BASE - 6),
    PRTE_ERR_NOT_IMPLEMENTED = (PRTE_ERR_BASE - 7),
    PRTE_ERR_NOT_SUPPORTED = (PRTE_ERR_BASE - 8),
    PRTE_ERR_INTERRUPTED = (PRTE_ERR_BASE - 9),
    PRTE_ERR_WOULD_BLOCK = (PRTE_ERR_BASE - 10),
    PRTE_ERR_IN_ERRNO = (PRTE_ERR_BASE - 11),
    PRTE_ERR_UNREACH = (PRTE_ERR_BASE - 12),
    PRTE_ERR_NOT_FOUND = (PRTE_ERR_BASE - 13),
    PRTE_EXISTS = (PRTE_ERR_BASE - 14), /* indicates that the specified object already exists */
    PRTE_ERR_TIMEOUT = (PRTE_ERR_BASE - 15),
    PRTE_ERR_NOT_AVAILABLE = (PRTE_ERR_BASE - 16),
    PRTE_ERR_PERM = (PRTE_ERR_BASE - 17), /* no permission */
    PRTE_ERR_VALUE_OUT_OF_BOUNDS = (PRTE_ERR_BASE - 18),
    PRTE_ERR_FILE_READ_FAILURE = (PRTE_ERR_BASE - 19),
    PRTE_ERR_FILE_WRITE_FAILURE = (PRTE_ERR_BASE - 20),
    PRTE_ERR_FILE_OPEN_FAILURE = (PRTE_ERR_BASE - 21),
    PRTE_ERR_PACK_MISMATCH = (PRTE_ERR_BASE - 22),
    PRTE_ERR_PACK_FAILURE = (PRTE_ERR_BASE - 23),
    PRTE_ERR_UNPACK_FAILURE = (PRTE_ERR_BASE - 24),
    PRTE_ERR_UNPACK_INADEQUATE_SPACE = (PRTE_ERR_BASE - 25),
    PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER = (PRTE_ERR_BASE - 26),
    PRTE_ERR_TYPE_MISMATCH = (PRTE_ERR_BASE - 27),
    PRTE_ERR_OPERATION_UNSUPPORTED = (PRTE_ERR_BASE - 28),
    PRTE_ERR_UNKNOWN_DATA_TYPE = (PRTE_ERR_BASE - 29),
    PRTE_ERR_BUFFER = (PRTE_ERR_BASE - 30),
    PRTE_ERR_DATA_TYPE_REDEF = (PRTE_ERR_BASE - 31),
    PRTE_ERR_DATA_OVERWRITE_ATTEMPT = (PRTE_ERR_BASE - 32),
    PRTE_ERR_MODULE_NOT_FOUND = (PRTE_ERR_BASE - 33),
    PRTE_ERR_TOPO_SLOT_LIST_NOT_SUPPORTED = (PRTE_ERR_BASE - 34),
    PRTE_ERR_TOPO_SOCKET_NOT_SUPPORTED = (PRTE_ERR_BASE - 35),
    PRTE_ERR_TOPO_CORE_NOT_SUPPORTED = (PRTE_ERR_BASE - 36),
    PRTE_ERR_NOT_ENOUGH_SOCKETS = (PRTE_ERR_BASE - 37),
    PRTE_ERR_NOT_ENOUGH_CORES = (PRTE_ERR_BASE - 38),
    PRTE_ERR_INVALID_PHYS_CPU = (PRTE_ERR_BASE - 39),
    PRTE_ERR_MULTIPLE_AFFINITIES = (PRTE_ERR_BASE - 40),
    PRTE_ERR_SLOT_LIST_RANGE = (PRTE_ERR_BASE - 41),
    PRTE_ERR_NETWORK_NOT_PARSEABLE = (PRTE_ERR_BASE - 42),
    PRTE_ERR_SILENT = (PRTE_ERR_BASE - 43),
    PRTE_ERR_NOT_INITIALIZED = (PRTE_ERR_BASE - 44),
    PRTE_ERR_NOT_BOUND = (PRTE_ERR_BASE - 45),
    PRTE_ERR_TAKE_NEXT_OPTION = (PRTE_ERR_BASE - 46),
    PRTE_ERR_PROC_ENTRY_NOT_FOUND = (PRTE_ERR_BASE - 47),
    PRTE_ERR_DATA_VALUE_NOT_FOUND = (PRTE_ERR_BASE - 48),
    PRTE_ERR_CONNECTION_FAILED = (PRTE_ERR_BASE - 49),
    PRTE_ERR_AUTHENTICATION_FAILED = (PRTE_ERR_BASE - 50),
    PRTE_ERR_COMM_FAILURE = (PRTE_ERR_BASE - 51),
    PRTE_ERR_SERVER_NOT_AVAIL = (PRTE_ERR_BASE - 52),
    PRTE_ERR_IN_PROCESS = (PRTE_ERR_BASE - 53),
    /* PMIx equivalents for notification support */
    PRTE_ERR_DEBUGGER_RELEASE = (PRTE_ERR_BASE - 54),
    PRTE_ERR_HANDLERS_COMPLETE = (PRTE_ERR_BASE - 55),
    PRTE_ERR_PARTIAL_SUCCESS = (PRTE_ERR_BASE - 56),
    PRTE_ERR_PROC_ABORTED = (PRTE_ERR_BASE - 57),
    PRTE_ERR_PROC_REQUESTED_ABORT = (PRTE_ERR_BASE - 58),
    PRTE_ERR_PROC_ABORTING = (PRTE_ERR_BASE - 59),
    PRTE_ERR_NODE_DOWN = (PRTE_ERR_BASE - 60),
    PRTE_ERR_NODE_OFFLINE = (PRTE_ERR_BASE - 61),
    PRTE_ERR_JOB_TERMINATED = (PRTE_ERR_BASE - 62),
    PRTE_ERR_PROC_RESTART = (PRTE_ERR_BASE - 63),
    PRTE_ERR_PROC_CHECKPOINT = (PRTE_ERR_BASE - 64),
    PRTE_ERR_PROC_MIGRATE = (PRTE_ERR_BASE - 65),
    PRTE_ERR_EVENT_REGISTRATION = (PRTE_ERR_BASE - 66),
    PRTE_ERR_HEARTBEAT_ALERT = (PRTE_ERR_BASE - 67),
    PRTE_ERR_FILE_ALERT = (PRTE_ERR_BASE - 68),
    PRTE_ERR_MODEL_DECLARED = (PRTE_ERR_BASE - 69),
    PRTE_PMIX_LAUNCH_DIRECTIVE = (PRTE_ERR_BASE - 70),
    PRTE_PMIX_LAUNCHER_READY = (PRTE_ERR_BASE - 71),
    PRTE_OPERATION_SUCCEEDED = (PRTE_ERR_BASE - 72),

    /* Error codes specific to PRRTE.
     *
     * Whichever group you append to, give the new code a string in
     * prte_strerror(), which lives in src/util/error.c -- NOT in
     * src/util/error_strings.c, which this comment used to name.  That file
     * holds the *state* strings (prte_job_state_to_str() and friends), so
     * following the old instruction put the string somewhere prte_strerror()
     * never looks and left the user reading "Unknown error" -- precisely the
     * failure the instruction exists to prevent.  test/unit/util catches it.
     */

    PRTE_ERR_RECV_LESS_THAN_POSTED = (PRTE_ERR_BASE - 101),
    PRTE_ERR_RECV_MORE_THAN_POSTED = (PRTE_ERR_BASE - 102),
    PRTE_ERR_NO_MATCH_YET = (PRTE_ERR_BASE - 103),
    PRTE_ERR_REQUEST = (PRTE_ERR_BASE - 104),
    PRTE_ERR_NO_CONNECTION_ALLOWED = (PRTE_ERR_BASE - 105),
    PRTE_ERR_CONNECTION_REFUSED = (PRTE_ERR_BASE - 106),
    PRTE_ERR_COMPARE_FAILURE = (PRTE_ERR_BASE - 107),
    PRTE_ERR_COPY_FAILURE = (PRTE_ERR_BASE - 108),
    PRTE_ERR_PROC_STATE_MISSING = (PRTE_ERR_BASE - 109),
    PRTE_ERR_PROC_EXIT_STATUS_MISSING = (PRTE_ERR_BASE - 110),
    PRTE_ERR_INDETERMINATE_STATE_INFO = (PRTE_ERR_BASE - 111),
    PRTE_ERR_NODE_FULLY_USED = (PRTE_ERR_BASE - 112),
    PRTE_ERR_INVALID_NUM_PROCS = (PRTE_ERR_BASE - 113),
    PRTE_ERR_ADDRESSEE_UNKNOWN = (PRTE_ERR_BASE - 114),
    PRTE_ERR_SYS_LIMITS_PIPES = (PRTE_ERR_BASE - 115),
    PRTE_ERR_PIPE_SETUP_FAILURE = (PRTE_ERR_BASE - 116),
    PRTE_ERR_SYS_LIMITS_CHILDREN = (PRTE_ERR_BASE - 117),
    PRTE_ERR_FAILED_GET_TERM_ATTRS = (PRTE_ERR_BASE - 118),
    PRTE_ERR_WDIR_NOT_FOUND = (PRTE_ERR_BASE - 119),
    PRTE_ERR_EXE_NOT_FOUND = (PRTE_ERR_BASE - 120),
    PRTE_ERR_PIPE_READ_FAILURE = (PRTE_ERR_BASE - 121),
    PRTE_ERR_EXE_NOT_ACCESSIBLE = (PRTE_ERR_BASE - 122),
    PRTE_ERR_FAILED_TO_START = (PRTE_ERR_BASE - 123),
    PRTE_ERR_FILE_NOT_EXECUTABLE = (PRTE_ERR_BASE - 124),
    PRTE_ERR_HNP_COULD_NOT_START = (PRTE_ERR_BASE - 125),
    PRTE_ERR_SYS_LIMITS_SOCKETS = (PRTE_ERR_BASE - 126),
    PRTE_ERR_SOCKET_NOT_AVAILABLE = (PRTE_ERR_BASE - 127),
    PRTE_ERR_SYSTEM_WILL_BOOTSTRAP = (PRTE_ERR_BASE - 128),
    PRTE_ERR_RESTART_LIMIT_EXCEEDED = (PRTE_ERR_BASE - 129),
    PRTE_ERR_INVALID_NODE_RANK = (PRTE_ERR_BASE - 130),
    PRTE_ERR_INVALID_LOCAL_RANK = (PRTE_ERR_BASE - 131),
    PRTE_ERR_UNRECOVERABLE = (PRTE_ERR_BASE - 132),
    PRTE_ERR_MEM_LIMIT_EXCEEDED = (PRTE_ERR_BASE - 133),
    PRTE_ERR_HEARTBEAT_LOST = (PRTE_ERR_BASE - 134),
    PRTE_ERR_PROC_STALLED = (PRTE_ERR_BASE - 135),
    PRTE_ERR_NO_APP_SPECIFIED = (PRTE_ERR_BASE - 136),
    PRTE_ERR_NO_EXE_SPECIFIED = (PRTE_ERR_BASE - 137),
    PRTE_ERR_COMM_DISABLED = (PRTE_ERR_BASE - 138),
    PRTE_ERR_FAILED_TO_MAP = (PRTE_ERR_BASE - 139),
    PRTE_ERR_SENSOR_LIMIT_EXCEEDED = (PRTE_ERR_BASE - 140),
    PRTE_ERR_ALLOCATION_PENDING = (PRTE_ERR_BASE - 141),
    PRTE_ERR_NO_PATH_TO_TARGET = (PRTE_ERR_BASE - 142),
    PRTE_ERR_OP_IN_PROGRESS = (PRTE_ERR_BASE - 143),
    PRTE_ERR_OPEN_CONDUIT_FAIL = (PRTE_ERR_BASE - 144),
    PRTE_ERR_DUPLICATE_MSG = (PRTE_ERR_BASE - 145),
    PRTE_ERR_OUT_OF_ORDER_MSG = (PRTE_ERR_BASE - 146),
    PRTE_ERR_FORCE_SELECT = (PRTE_ERR_BASE - 147),
    PRTE_ERR_JOB_CANCELLED = (PRTE_ERR_BASE - 148),
    PRTE_ERR_CONDUIT_SEND_FAIL = (PRTE_ERR_BASE - 149),
    PRTE_ERR_JSON_PARSE_FAILURE = (PRTE_ERR_BASE - 150),
    PRTE_ERR_SLURM_QUERY_FAILURE = (PRTE_ERR_BASE - 151),
    PRTE_ERR_SLURM_BAD_JOB_STATUS = (PRTE_ERR_BASE - 152),
    PRTE_ERR_SLURM_SUBMIT_FAILURE = (PRTE_ERR_BASE - 153),
    PRTE_ERR_SLURM_CANCEL_FAILURE = (PRTE_ERR_BASE - 154),
    PRTE_ERR_SLURM_SHRINK_FAILURE = (PRTE_ERR_BASE - 155),
    PRTE_ERR_PRELOAD_CONFLICT = (PRTE_ERR_BASE - 156),
    PRTE_ERR_SLURM_UPDATE_FAILURE = (PRTE_ERR_BASE - 157),

    /* Not an error code.  Nothing returns it, nothing tests for it, and
     * prte_strerror() deliberately has no case for it: it is one past the
     * last assigned offset, and it exists so that the code that sweeps this
     * list has a bound it does not have to guess.
     *
     * Bump it in the same edit that appends a code above.  It sits here,
     * adjacent to where you are appending, for exactly that reason -- the
     * two copies of this bound that used to live in test/unit/include and
     * test/unit/util both went stale on the first code added after they
     * were written, which silently stopped the string sweep one code short
     * of the end.  Forgetting is now a test failure rather than a quiet
     * loss of coverage: test/unit/util checks that PRTE_ERR_MAX itself has
     * no string and that the code just above it does, so a bound that is
     * either too low or too high is caught.
     *
     * There was an earlier PRTE_ERR_MAX, removed because nothing consumed
     * it -- and because, computed from a base whose sign had been dropped,
     * it evaluated to PRTE_SUCCESS.  This one has consumers, and a test
     * that notices when it lies.
     */
    PRTE_ERR_MAX = (PRTE_ERR_BASE - 158)
};

END_C_DECLS

#endif /* PRTE_CONSTANTS_H */
