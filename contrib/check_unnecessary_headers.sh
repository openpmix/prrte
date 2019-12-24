#!/bin/sh
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
#
#
# Some grep/sed mojo may be of interest to others...
# Find certain unnecessary headers, and remove, if not used...
#

function del_header()
{
    file=$1
    header=`echo $2 | sed 's/\//\\\\\//g'`
    line=`grep -n "#include \"$2" $file | cut -f1 -d':' | head -n1`

    if [ "x$line" = "x" ] ; then
        echo Can not find pattern $header file $file -- will not delete
        return
    fi

    # Remove the header including any characters at end of the line   MULTI_LINE COMMENTS...?
    sed -i -e "/#include \"$header\".*/d" $file
}


#
# In Subdirectory DIR, search for headers provided in array SEARCH_HEADER
# SEARCH_HEADER must initilized with a[0]="header_file.h  typdefs  macros  functions"
#
# If any of the typedefs, macros or functions show up, then
# including the header is valid required, otherwise del_header
#
#
# XXX Dont ask, why it does NOT take the SEARCH_HEADER as parameter -- seems not to work with arrays
#
function delete_unnessary_header()
{
    DIR=$1
    i=0
    while [ "x${SEARCH_HEADER[i]}" != "x" ] ; do
        HEADER=`echo ${SEARCH_HEADER[i]} | cut -f 1 -d' '`
        HEADER_BASENAME=`basename $HEADER`
        for file in `find $DIR -type f '(' -name '*.[cChysSfF]' -o \
                                           -iname '*.cc' -o -name '*.cpp' -o \
                                           -iname '*.f77' -o -iname '*.f90' ')' | sort | xargs grep -n $HEADER | cut -f1 -d':' | sort | uniq` ; do
            need_h=0
            ARGS=`echo ${SEARCH_HEADER[i]} | cut -f2- -d' '`
            for arg in $ARGS ; do
                # echo $HEADER $file $arg
                # If the poor argument is part of the header's name, how to find it? Well, need to play with grep!
                if test "x${arg}.h" = "x${HEADER_BASENAME}" ; then
                    grep $arg $file | grep -qv $HEADER && need_h=1
                else
                    grep -q $arg $file && need_h=1
                fi
                [ $need_h -eq 1 ] && break
            done
            if [ $need_h -eq 0 ] ; then
                echo -e Delete $HEADER  from \\t $file # as we did not find any of $ARGS
                del_header $file $HEADER
            fi
        done
        i=$(( i + 1 ))
    done
}


#
# First element of the array is the header file in question, all following
# strings are functions, types, macros that are being checked for...
#
# WARNING: variables should NOT be separated by MORE than 1 space
#
SEARCH_HEADER[0]="opal/align.h OPAL_ALIGN_PTR OPAL_ALIGN"
SEARCH_HEADER[1]="opal/class/opal_atomic_lifo.h opal_atomic_lifo_t opal_atomic_lifo_is_empty opal_atomic_lifo_push opal_atomic_lifo_pop"
SEARCH_HEADER[2]="opal/class/opal_bitmap.h opal_bitmap_t opal_bitmap_set_max_size opal_bitmap_init opal_bitmap_set_bit opal_bitmap_clear_bit opal_bitmap_is_set_bi opal_bitmap_find_and_set_first_unset_bit opal_bitmap_clear_all_bits opal_bitmap_set_all_bits opal_bitmap_size opal_bitmap_copy"
SEARCH_HEADER[3]="opal/class/opal_free_list.h opal_free_list_t opal_free_list_item_t opal_free_list_init opal_free_list_grow OPAL_FREE_LIST_GET OPAL_FREE_LIST_WAIT OPAL_FREE_LIST_RETURN"
SEARCH_HEADER[4]="opal/class/opal_graph.h opal_graph_vertex_t opal_graph_edge_t opal_adjacency_list_t opal_graph_t opal_graph_copy_vertex_data opal_graph_free_vertex_data opal_graph_alloc_vertex_data vertex_distance_from_t opal_graph_add_vertex opal_graph_remove_vertex opal_graph_add_edge opal_graph_remove_edge opal_graph_adjacent opal_graph_get_order opal_graph_get_size opal_graph_find_vertex opal_graph_get_graph_vertices opal_graph_get_adjacent_vertices opal_graph_duplicate opal_graph_spf opal_graph_dijkstra opal_graph_print"
SEARCH_HEADER[5]="opal/class/opal_hash_table.h opal_hash_table_t opal_hash_table_init opal_hash_table_get_size opal_hash_table_remove_all opal_hash_table_get_value_uint32 opal_hash_table_set_value_uint32 opal_hash_table_remove_value_uint32 opal_hash_table_get_value_uint64 opal_hash_table_set_value_uint64 opal_hash_table_remove_value_uint64 opal_hash_table_get_value_ptr opal_hash_table_set_value_ptr opal_hash_table_remove_value_ptr opal_hash_table_get_first_key_uint32 opal_hash_table_get_next_key_uint32 opal_hash_table_get_first_key_uint64 opal_hash_table_get_next_key_uint64"
SEARCH_HEADER[6]="opal/class/opal_list.h opal_list_t opal_list_item_t opal_list_get_next opal_list_get_prev opal_list_is_empty opal_list_get_first opal_list_get_last opal_list_get_begin opal_list_get_end opal_list_get_size opal_list_remove_item opal_list_append opal_list_prepend opal_list_remove_first opal_list_remove_last opal_list_insert_pos opal_list_insert opal_list_join opal_list_splice opal_list_sort opal_list_item_compare_fn_t"
SEARCH_HEADER[7]="opal/class/opal_object.h opal_object_t opal_class_t opal_construct_t opal_destruct_t OPAL_OBJ_STATIC_INIT OBJ_CLASS OBJ_CLASS_INSTANCE OBJ_CLASS_DECLARATION OBJ_NEW OBJ_RETAIN OBJ_RELEASE OBJ_CONSTRUCT OBJ_DESTRUCT opal_class_initialize opal_class_finalize opal_obj_run_constructors opal_obj_run_destructors opal_obj_new opal_obj_update"
SEARCH_HEADER[8]="opal/class/opal_pointer_array.h opal_pointer_array_t opal_pointer_array_init opal_pointer_array_add opal_pointer_array_set_item opal_pointer_array_get_item opal_pointer_array_get_size opal_pointer_array_set_size opal_pointer_array_test_and_set_item opal_pointer_array_remove_all"
SEARCH_HEADER[9]="opal/class/opal_value_array.h opal_value_array_t opal_value_array_init opal_value_array_reserve opal_value_array_get_size opal_value_array_set_size OPAL_VALUE_ARRAY_GET_ITEM opal_value_array_get_item OPAL_VALUE_ARRAY_SET_ITEM opal_value_array_set_item opal_value_array_append_item opal_value_array_remove_item OPAL_VALUE_ARRAY_GET_BASE"
SEARCH_HEADER[10]="opal/constants.h OPAL_SUCCESS OPAL_ERROR OPAL_ERR_ OPAL_EXISTS"
SEARCH_HEADER[11]="opal/dss/dss.h opal_dss_open opal_dss_close opal_dss_t opal_dss"
SEARCH_HEADER[12]="opal/dss/dss_internal.h OPAL_DSS_DEFAULT_INITIAL_SIZE OPAL_DSS_DEFAULT_THRESHOLD_SIZE DSS_TYPE_SIZE_T DSS_TYPE_BOOL DSS_TYPE_INT DSS_TYPE_UINT DSS_TYPE_PID_T UNPACK_SIZE_MISMATCH opal_dss_type_info_t opal_dss_initialized opal_dss_debug opal_dss_verbose opal_dss_initial_size opal_dss_threshold_size opal_dss_types opal_dss_num_reg_types opal_dss_set opal_dss_get opal_dss_pack opal_dss_unpack opal_dss_copy opal_dss_compare opal_dss_print opal_dss_dump opal_dss_size opal_dss_peek opal_dss_unload opal_dss_load opal_dss_copy_payload opal_dss_register opal_dss_release opal_dss_lookup_data_type opal_dss_dump_data_types opal_dss_pack_buffer opal_dss_unpack_buffer opal_dss_std_copy opal_dss_compare_ opal_dss_std_size opal_dss_size_ opal_dss_print_ opal_dss_std_release opal_dss_std_obj_release opal_dss_release_byte_object opal_dss_buffer_extend opal_dss_too_small opal_dss_find_type opal_dss_store_data_type opal_dss_get_data_type"
SEARCH_HEADER[13]="opal/dss/dss_types.h OPAL_DATA_TYPE_T OPAL_DSS_ID_MAX OPAL_DSS_ID_INVALID OPAL_UNDEF OPAL_BYTE OPAL_BOOL OPAL_STRING OPAL_SIZE OPAL_PID OPAL_INT OPAL_UINT OPAL_BYTE_OBJECT OPAL_DATA_TYPE OPAL_NULL OPAL_DATA_VALUE OPAL_PSTAT OPAL_DSS_ID_DYNAMIC OPAL_VALUE1_GREATER OPAL_VALUE2_GREATER OPAL_EQUAL opal_dss_value_t OPAL_DATA_VALUE_EMPTY OPAL_PSTAT_MAX_STRING_LEN opal_pstats_t OPAL_DSS_STRUCTURED OPAL_DSS_UNSTRUCTURED OPAL_DSS_BUFFER_NON_DESC OPAL_DSS_BUFFER_FULLY_DESC OPAL_DSS_BUFFER_TYPE_HTON OPAL_DSS_BUFFER_TYPE_NTOH opal_buffer_t"
SEARCH_HEADER[14]="opal/hash_string.h OPAL_HASH_STR"
SEARCH_HEADER[15]="opal/mca/crs.h OPAL_CRS_ opal_crs"
SEARCH_HEADER[16]="opal/opal_socket_errno.h opal_socket_errno"
SEARCH_HEADER[17]="opal/prefetch.h OPAL_LIKELY OPAL_UNLIKELY OPAL_PREFETCH"
SEARCH_HEADER[18]="opal/runtime/opal_cr.h OPAL_CR_ opal_cr_ OPAL_CHECKPOINT_CMD_ OPAL_CR_STATUS_"
SEARCH_HEADER[19]="opal/runtime/opal_progress.h opal_progress"
SEARCH_HEADER[20]="opal_stdint.h" # "int8_t int16_t int32_t int64_t intptr_t SIZE_MAX PRId PRIi PRIo PRIu PRIx PRIX PRIsize_t"
SEARCH_HEADER[21]="opal/threads/condition.h opal_condition_t opal_condition_wait opal_condition_timedwait opal_condition_signal opal_condition_broadcast"
SEARCH_HEADER[22]="opal/threads/mutex.h opal_uses_threads opal_mutex_check_locks opal_mutex_t opal_mutex_trylock opal_mutex_lock opal_mutex_unlock opal_mutex_atomic_trylock opal_mutex_atomic_lock opal_mutex_atomic_unlock opal_using_threads OPAL_THREAD_LOCK OPAL_THREAD_TRYLOCK OPAL_THREAD_UNLOCK OPAL_THREAD_SCOPED_LOCK OPAL_THREAD_ADD OPAL_HAVE_ATOMIC_CMPSET"
SEARCH_HEADER[23]="opal/threads/threads.h opal_thread_fn_t opal_thread_t opal_thread_start opal_thread_join opal_thread_self_compare opal_thread_get_self"
SEARCH_HEADER[24]="opal/types.h ompi_ptr_t ompi_iov_base_ptr_t opal_socklen_t hton64 ntoh64 ompi_ptr_ptol ompi_ptr_ltop opal_swap_bytes2 opal_swap_bytes4 opal_swap_bytes8"
SEARCH_HEADER[25]="opal/util/arch.h OPAL_ARCH_ opal_arch_compute_local_id opal_arch_checkmask opal_arch_isbigendian opal_arch_ldisintel opal_arch_setmask"
SEARCH_HEADER[26]="opal/util/argv.h opal_argv_append opal_argv_append_nosize opal_argv_append_unique_nosize opal_argv_free opal_argv_split opal_argv_split_with_empty opal_argv_count opal_argv_join opal_argv_join_range opal_argv_len opal_argv_copy opal_argv_delete opal_argv_insert"
SEARCH_HEADER[27]="opal/util/basename.h opal_basename opal_dirname"
SEARCH_HEADER[28]="opal/util/bit_ops.h opal_hibit opal_cube_dim"
SEARCH_HEADER[29]="opal/util/cmd_line.h opal_cmd_line_t OPAL_CMD_LINE_TYPE_ opal_cmd_line_type_t opal_cmd_line_init_t opal_cmd_line_create opal_cmd_line_make_opt_mca opal_cmd_line_make_opt opal_cmd_line_make_opt3 opal_cmd_line_parse opal_cmd_line_get_usage_msg opal_cmd_line_is_taken opal_cmd_line_get_argc opal_cmd_line_get_argv opal_cmd_line_get_ninsts opal_cmd_line_get_param opal_cmd_line_get_tail"
SEARCH_HEADER[30]="opal/util/convert.h opal_size2int"
SEARCH_HEADER[31]="opal/util/daemon_init.h opal_daemon_init"
SEARCH_HEADER[32]="opal/util/error.h opal_perror opal_strerror opal_strerror_r opal_err2str_fn_t opal_error_register"
SEARCH_HEADER[33]="opal/util/if.h IF_NAMESIZE opal_ifnametoaddr opal_ifaddrtoname opal_ifnametoindex opal_ifnametokindex opal_ifindextokindex opal_ifcount opal_ifbegin opal_ifnext opal_ifindextoname opal_ifkindextoname opal_ifindextoaddr opal_ifindextomask opal_ifindextoflags opal_ifislocal opal_iffinalize"
SEARCH_HEADER[34]="opal/util/net.h opal_net_init opal_net_finalize opal_net_prefix2netmask opal_net_islocalhost opal_net_samenetwork opal_net_addr_isipv4public opal_net_get_hostname opal_net_get_port"
SEARCH_HEADER[35]="opal/util/opal_environ.h opal_environ_merge opal_setenv opal_unsetenv opal_home_directory opal_tmp_directory environ"
SEARCH_HEADER[36]="opal/util/opal_getcwd.h opal_getcwd"
SEARCH_HEADER[37]="opal/util/os_dirpath.h opal_os_dirpath_create opal_os_dirpath_is_empty opal_os_dirpath_access opal_os_dirpath_destroy"
SEARCH_HEADER[38]="opal/util/os_path.h opal_os_path opal_make_filename_os_friendly"
SEARCH_HEADER[39]="opal/util/output.h opal_output_stream_t opal_output_init opal_output_finalize opal_output_open opal_output_reopen opal_output_switch opal_output_reopen_all opal_output_close opal_output opal_output_verbose opal_output_vverbose opal_output_string opal_output_vstring opal_output_set_verbosity opal_output_get_verbosity opal_output_set_output_file_info OPAL_OUTPUT OPAL_OUTPUT_VERBOSE"
SEARCH_HEADER[40]="opal/util/path.h opal_path_find opal_path_findv opal_path_is_absolute opal_find_absolute_path opal_path_access"
SEARCH_HEADER[41]="opal/util/printf.h snprintf vsnprintf"
SEARCH_HEADER[42]="opal/util/show_help.h opal_show_help_init opal_show_help_finalize opal_show_help opal_show_vhelp opal_show_help_string opal_show_help_finish_parsing"
SEARCH_HEADER[43]="opal/util/strncpy.h opal_strncpy"
SEARCH_HEADER[44]="opal/util/sys_limits.h opal_sys_limits opal_util_init_sys_limits"
SEARCH_HEADER[45]=""

delete_unnessary_header .

####################################
SEARCH_HEADER[0]="prrte/mca/errmgr/errmgr.h PRRTE_ERROR_NAME PRRTE_ERROR_LOG prrte_errmgr_base_log prrte_errmgr"
SEARCH_HEADER[1]="prrte/mca/ess/ess.h prrte_ess"
SEARCH_HEADER[2]="prrte/mca/filem/filem.h prrte_filem PRRTE_FILEM_TYPE_ PRRTE_FILEM_MOVE_ prrte_filem_base_process_set_1_0_0_t prrte_filem_base_process_set_t prrte_filem_base_file_set_1_0_0_t prrte_filem_base_file_set_t prrte_filem_base_request_1_0_0_t prrte_filem_base_request_t prrte_filem_base_component_2_0_0_t prrte_filem_base_component_t prrte_filem_base_module_1_0_0_t prrte_filem_base_module_t"
SEARCH_HEADER[3]="prrte/mca/grpcomm/grpcomm.h prrte_grpcomm"
SEARCH_HEADER[4]="prrte/mca/iof/iof.h prrte_iof"
SEARCH_HEADER[5]="prrte/mca/iof/iof_types.h prrte_iof_tag_t PRRTE_IOF_"
SEARCH_HEADER[6]="prrte/mca/notifier/notifier.h PRRTE_NOTIFIER_MAX_BUF PRRTE_NOTIFIER_INFRA PRRTE_NOTIFIER_WARNING prrte_notifier"
SEARCH_HEADER[7]="prrte/mca/odls/base/base.h prrte_odls_base_open prrte_odls_base_t prrte_odls_base prrte_base_default_waitpid_fired"
SEARCH_HEADER[8]="prrte/mca/odls/odls.h prrte_odls_base_module_1_3_0_t prrte_odls_base_module_t prrte_odls_base_component_2_0_0_t prrte_odls_base_component_t prrte_odls"
SEARCH_HEADER[9]="prrte/mca/odls/odls_types.h prrte_daemon_cmd_flag_t PRRTE_DAEMON_ prrte_odls_child_t prrte_odls_job_t"
SEARCH_HEADER[10]="prrte/mca/oob/oob.h mca_oob_1_0_0_t mca_oob_t mca_oob_base_component_2_0_0_t mca_oob_base_component_t mca_oob"
SEARCH_HEADER[11]="prrte/mca/plm/plm.h prrte_plm"
SEARCH_HEADER[12]="prrte/mca/plm/plm_types.h prrte_exit_code_t PRRTE_EXIT_CODE_T prrte_proc_state_t PRRTE_PROC_STATE_ prrte_job_state_t PRRTE_JOB_STATE_ PRRTE_JOB_NEVER_LAUNCHED prrte_node_state_t PRRTE_NODE_STATE_ prrte_plm_cmd_flag_t PRRTE_PLM_CMD PRRTE_PLM_LAUNCH_JOB_CMD PRRTE_PLM_UPDATE_PROC_STATE PRRTE_PLM_HEARTBEAT_CMD"
###
SEARCH_HEADER[13]="prrte/mca/rmaps/rmaps.h prrte_rmaps"
SEARCH_HEADER[14]="prrte/mca/rmaps/rmaps_types.h PRRTE_RMAPS_ prrte_job_map_t"
###
SEARCH_HEADER[15]="prrte/mca/rml/base/rml_contact.h prrte_rml_base_get_contact_info prrte_rml_base_update_contact_info prrte_rml_base_parse_uris"
SEARCH_HEADER[16]="prrte/mca/rml/rml.h prrte_rml"
SEARCH_HEADER[17]="prrte/mca/rml/rml_types.h PRRTE_RML_TAG_ prrte_rml_tag_t prrte_rml_cmd_flag_t PRRTE_RML_CMD PRRTE_RML_UPDATE_CMD PRRTE_RML_NON_PERSISTENT PRRTE_RML_PEEK PRRTE_RML_TRUNC PRRTE_RML_ALLOC PRRTE_RML_PERSISTENT PRRTE_RML_FLAG_RECURSIVE_CALLBACK prrte_rml_exception_t PRRTE_RML_PEER_UNREACH PRRTE_RML_PEER_DISCONNECTED"
###
SEARCH_HEADER[18]="prrte/runtime/prrte_data_server.h prrte_data_server_init prrte_data_server_finalize prrte_data_server_cmd_t PRRTE_DATA_SERVER_"
SEARCH_HEADER[19]="prrte/runtime/prrte_globals.h prrte_debug_verbosity prrte_prohibited_session_dirs prrte_xml_output prrte_help_want_aggregate PRRTE_NAME_WILDCARD prrte_name_wildcard PRRTE_NAME_INVALID prrte_name_invalid PRRTE_PROC_MY_NAME PRRTE_PROC_MY_HNP PRRTE_PROC_MY_DAEMON prrte_in_parallel_debugger PRRTE_GLOBAL_ARRAY_BLOCK_SIZE PRRTE_GLOBAL_ARRAY_MAX_SIZE PRRTE_ERROR_DEFAULT_EXIT_CODE PRRTE_UPDATE_EXIT_STATUS PRRTE_COMPUTE_TIME_DIFF prrte_app_context_t prrte_node_t prrte_job_controls_t PRRTE_JOB_CONTROL prrte_job_t prrte_proc_t prrte_attr_t prrte_nid_t prrte_pmap_t prrte_jmap_t prrte_get_job_data_object prrte_timing prrte_debug_daemons_flag prrte_debug_daemons_file_flag prrte_leave_session_attached prrte_do_not_launch prted_spin_flag prrte_static_ports prrte_contiguous_nodes prrte_keep_fqdn_hostnames prrte_show_resolved_nodenames prted_debug_failure prrte_homogeneous_nodes prrte_hetero_apps prrte_never_launched prrte_devel_level_output prrte_launch_environ prrte_hnp_is_allocated prrte_allocation_required prrte_launch_agent prted_cmd_line prrte_debugger_daemon prrte_enable_debug_cospawn_while_running prrte_debugger_check_rate prrte_exit orteds_exit prrte_exit_status prrte_abnormal_term_ordered prrte_routing_is_enabled prrte_job_term_ordered  prrte_heartbeat_rate prrte_startup_timeout prrte_timeout_usec_per_proc prrte_max_timeout prrte_default_hostfile prrte_tree_launch_cmd prrte_job_data prrte_node_pool prrte_clean_output prrte_send_profile prrte_nidmap prrte_jobmap prrte_local_children prrte_local_jobdata prrte_forward_job_control prrte_tag_output prrte_tag_output prrte_timestamp_output prrte_output_filename prrte_xterm prrte_rsh_agent"
SEARCH_HEADER[20]="prrte/runtime/runtime.h prrte_version_string prrte_initialized prrte_finalizing prrte_debug_output prrte_debug_flag PRRTE_NON_TOOL PRRTE_TOOL prrte_init prrte_register_params prrte_finalize"
###
SEARCH_HEADER[21]="prrte/types.h prrte_std_cntr_t PRRTE_STD_CNTR_ prrte_local_rank_t prrte_node_rank_t PRRTE_LOCAL_RANK PRRTE_NODE_RANK PRRTE_LOCAL_RANK_MAX PRRTE_NODE_RANK_MAX PRRTE_LOCAL_RANK_INVALID PRRTE_NODE_RANK_INVALID prrte_jobid_t PRRTE_JOBID_ prrte_vpid_t PRRTE_VPID_ PRRTE_PROCESS_NAME_HTON PRRTE_PROCESS_NAME_NTOH PRRTE_NAME_ARGS PRRTE_JOBID_INVALID PRRTE_VPID_INVALID PRRTE_JOBID_WILDCARD PRRTE_VPID_WILDCARD prrte_process_name_t prrte_iov_base_ptr_t PRRTE_STD_CNTR PRRTE_NAME PRRTE_VPID PRRTE_JOBID PRRTE_NODE_STATE PRRTE_PROC_STATE PRRTE_JOB_STATE PRRTE_EXIT_CODE PRRTE_VALUE PRRTE_APP_CONTEXT PRRTE_NODE_DESC PRRTE_SLOT_DESC PRRTE_JOB PRRTE_NODE PRRTE_PROC PRRTE_JOB_MAP PRRTE_RML_TAG PRRTE_DAEMON_CMD PRRTE_GRPCOMM_MODE PRRTE_IOF_TAG"
###
SEARCH_HEADER[22]="prrte/util/hnp_contact.h prrte_hnp_contact_t prrte_write_hnp_contact_file prrte_read_hnp_contact_file prrte_list_local_hnps"
###
SEARCH_HEADER[23]="prrte/util/name_fns.h PRRTE_NS_CMP_ prrte_ns_cmp_bitmask_t prrte_util_print_name_args PRRTE_NAME_PRINT prrte_util_print_jobids PRRTE_JOBID_PRINT prrte_util_print_vpids PRRTE_VPID_PRINT prrte_util_print_job_family PRRTE_JOB_FAMILY_PRINT prrte_util_print_local_jobid PRRTE_LOCAL_JOBID_PRINT PRRTE_JOB_FAMILY PRRTE_HNP_NAME_FROM_JOB PRRTE_LOCAL_JOBID PRRTE_CONSTRUCT_LOCAL_JOBID PRRTE_PROC_IS_DAEMON prrte_namelist_t prrte_util_convert_ prrte_util_create_process_name prrte_util_compare_name_fields prrte_util_hash_name"
###
SEARCH_HEADER[24]="prrte/util/prrte_wait.h prrte_trigger_event_t prrte_wait_enable prrte_wait_disable prrte_waitpid prrte_wait_cb prrte_wait_event PRRTE_PROGRESSED_WAIT prrte_trigger_event prrte_message_event_t PRRTE_MESSAGE_EVENT_DELAY PRRTE_MESSAGE_EVENT prrte_notify_event_t PRRTE_DETECT_TIMEOUT PRRTE_TIMER_EVENT prrte_wait_init prrte_wait_kill prrte_wait_finalize"
###
SEARCH_HEADER[25]="prrte/util/parse_options.h prrte_util_parse_range_options"
###
SEARCH_HEADER[26]="prrte/util/proc_info.h PRRTE_MAX_HOSTNAME_SIZE prrte_proc_info_t prrte_process_info prrte_proc_info prrte_proc_info_finalize"
###
SEARCH_HEADER[27]="prrte/util/session_dir.h prrte_session_dir"
###
SEARCH_HEADER[28]="prrte/util/show_help.h prrte_show_help_init prrte_show_help_finalize prrte_show_help prrte_show_help_recv"
###
SEARCH_HEADER[29]=""

delete_unnessary_header .

####################################
SEARCH_HEADER[0]="ompi/attribute/attribute.h ATTR_HASH_SIZE OMPI_KEYVAL_PREDEFINED OMPI_KEYVAL_F77 ompi_attribute_type_t ompi_mpi1_fortran_copy_attr_function ompi_mpi1_fortran_delete_attr_function ompi_mpi2_fortran_copy_attr_function ompi_mpi2_fortran_delete_attr_function MPI_Comm_internal_copy_attr_function MPI_Type_internal_copy_attr_function MPI_Win_internal_copy_attr_function ompi_attribute_keyval_destructor_fn_t ompi_attribute_fn_ptr_union_t ompi_attribute_fortran_ptr_t ompi_attribute_keyval_t ompi_attr_hash_init ompi_attr_init ompi_attr_finalize ompi_attr_create_keyval ompi_attr_free_keyval ompi_attr_set_c ompi_attr_set_fortran_mpi1 ompi_attr_set_fortran_mpi2 ompi_attr_get_c ompi_attr_get_fortran_mpi1 ompi_attr_get_fortran_mpi2 ompi_attr_delete ompi_attr_copy_all ompi_attr_delete_all ompi_attr_create_predefined ompi_attr_free_predefined"
SEARCH_HEADER[1]="ompi/class/ompi_free_list.h ompi_free_list_item_init_fn_t ompi_free_list_t ompi_free_list_item_t ompi_free_list_init_ex ompi_free_list_init ompi_free_list_init_ex_new ompi_free_list_init_new ompi_free_list_grow ompi_free_list_resize ompi_free_list_pos_t OMPI_FREE_LIST_POS_BEGINNING ompi_free_list_parse OMPI_FREE_LIST_GET OMPI_FREE_LIST_WAIT __ompi_free_list_wait OMPI_FREE_LIST_RETURN"
SEARCH_HEADER[2]="ompi/class/ompi_rb_tree.h ompi_rb_tree_nodecolor_t ompi_rb_tree_node_t ompi_rb_tree_comp_fn_t ompi_rb_tree_t ompi_rb_tree_condition_fn_t ompi_rb_tree_action_fn_t ompi_rb_tree_construct ompi_rb_tree_destruct ompi_rb_tree_init ompi_rb_tree_insert ompi_rb_tree_find_with ompi_rb_tree_find ompi_rb_tree_delete ompi_rb_tree_destroy ompi_rb_tree_traverse ompi_rb_tree_size"
SEARCH_HEADER[3]="ompi/class/ompi_seq_tracker.h ompi_seq_tracker_range_t ompi_seq_tracker_t ompi_seq_tracker_check_duplicate ompi_seq_tracker_insert ompi_seq_tracker_copy"
SEARCH_HEADER[4]="ompi/communicator/communicator.h MPI_Comm MPI_COMM_WORLD ompi_communicator_t OMPI_COMM_INTER OMPI_COMM_CART OMPI_COMM_GRAPH OMPI_COMM_NAMEISSET OMPI_COMM_ISFREED OMPI_COMM_INTRINSIC OMPI_COMM_DYNAMIC OMPI_COMM_INVALID OMPI_COMM_PML_ADDED OMPI_COMM_IS_ OMPI_COMM_SET_ OMPI_COMM_ALLGATHER_TAG OMPI_COMM_BARRIER_TAG OMPI_COMM_ALLREDUCE_TAG OMPI_COMM_CID_ OMPI_COMM_BLOCK_ ompi_predefined_communicator_t ompi_mpi_comm_parent ompi_mpi_comm_world ompi_mpi_comm_self ompi_mpi_comm_null ompi_comm_invalid ompi_comm_rank ompi_comm_size ompi_comm_remote_size ompi_comm_get_cid ompi_comm_lookup ompi_comm_peer_lookup ompi_comm_peer_invalid ompi_comm_init ompi_comm_link_function ompi_comm_group ompi_comm_create ompi_topo_create ompi_comm_split ompi_comm_dup ompi_comm_compare ompi_comm_free ompi_comm_allocate ompi_comm_nextcid ompi_comm_finalize ompi_comm_set ompi_comm_get_rprocs ompi_comm_overlapping_groups ompi_comm_determine_first ompi_comm_activate ompi_comm_dump ompi_comm_set_name ompi_comm_reg_init ompi_comm_reg_finalize ompi_comm_num_dyncomm ompi_mpi_cxx_comm_errhandler_invoke"
SEARCH_HEADER[5]="ompi/datatype/convertor.h OMPI_COMM_INTER OMPI_COMM_CART OMPI_COMM_GRAPH OMPI_COMM_NAMEISSET OMPI_COMM_ISFREED OMPI_COMM_INTRINSIC OMPI_COMM_DYNAMIC OMPI_COMM_INVALID OMPI_COMM_PML_ADDED OMPI_COMM_IS_ OMPI_COMM_SET_ OMPI_COMM_ALLGATHER_TAG OMPI_COMM_BARRIER_TAG OMPI_COMM_ALLREDUCE_TAG OMPI_COMM_CID_ OMPI_COMM_BLOCK_ ompi_predefined_communicator_t ompi_mpi_comm_parent ompi_mpi_comm_null ompi_comm_invalid ompi_comm_rank ompi_comm_size ompi_comm_remote_size ompi_comm_get_cid ompi_comm_lookup ompi_comm_peer_lookup ompi_comm_peer_invalid ompi_comm_init ompi_comm_link_function ompi_comm_group ompi_comm_create ompi_topo_create ompi_comm_split ompi_comm_dup ompi_comm_compare ompi_comm_free ompi_comm_allocate ompi_comm_nextcid ompi_comm_finalize ompi_comm_set ompi_comm_get_rprocs ompi_comm_overlapping_groups ompi_comm_determine_first ompi_comm_activate ompi_comm_dump ompi_comm_set_name ompi_comm_reg_init ompi_comm_reg_finalize ompi_comm_num_dync CONVERTOR_DATATYPE_MASK CONVERTOR_SEND_CONVERSION CONVERTOR_RECV CONVERTOR_SEND CONVERTOR_HOMOGENEOUS CONVERTOR_NO_OP CONVERTOR_WITH_CHECKSUM CONVERTOR_TYPE_MASK CONVERTOR_STATE_START CONVERTOR_STATE_COMPLETE CONVERTOR_STATE_ALLOC CONVERTOR_COMPLETED ompi_convertor_t ompi_convertor_master_t dt_stack_t DT_STATIC_STACK_SIZE ompi_convertor_get_checksum ompi_convertor_pack ompi_convertor_unpack ompi_convertor_create ompi_convertor_cleanup ompi_convertor_need_buffers ompi_convertor_get_packed_size ompi_convertor_get_unpacked_size ompi_convertor_get_current_pointer ompi_convertor_prepare_for_send ompi_convertor_copy_and_prepare_for_send ompi_convertor_prepare_for_recv ompi_convertor_copy_and_prepare_for_recv ompi_convertor_raw ompi_convertor_set_position_nocheck ompi_convertor_set_position ompi_convertor_personalize ompi_convertor_clone ompi_convertor_clone_with_position ompi_convertor_dump ompi_ddt_dump_stack ompi_convertor_generic_simple_position MPI_Datatype"
SEARCH_HEADER[6]="ompi/datatype/datatype.h MPI_Datatype DT_MAX_PREDEFINED DT_FLAG_ MAX_DT_COMPONENT_COUNT opal_ddt_count_t dt_type_desc_t ompi_datatype_t ompi_predefined_datatype_t ompi_ddt_init ompi_ddt_finalize ompi_ddt_create_ ompi_ddt_duplicate ompi_ddt_is_predefined ompi_ddt_create_from_packed_description"
SEARCH_HEADER[7]="ompi/datatype/datatype_internal.h DDT_DUMP_STACK DT_ ddt_elem_id_description ddt_elem_desc ddt_elem_desc_t ddt_loop_desc ddt_loop_desc_t ddt_endloop_desc ddt_endloop_desc_t dt_elem_desc CREATE_LOOP_START CREATE_LOOP_END CREATE_ELEM ompi_complex_float_t ompi_complex_double_t ompi_complex_long_double_t ompi_ddt_basicDatatypes BASIC_DDT_FROM_ELEM ompi_ddt_default_convertors_init ompi_ddt_default_convertors_fini SAVE_STACK PUSH_STACK ompi_ddt_safeguard_pointer_debug_breakpoint OMPI_DDT_SAFEGUARD_POINTER GET_FIRST_NON_LOOP UPDATE_INTERNAL_COUNTERS ompi_ddt_print_args"
SEARCH_HEADER[8]="ompi/errhandler/errhandler.h OMPI_ERRHANDLER_LANG_ ompi_errhandler_lang_t OMPI_ERRHANDLER_TYPE_ ompi_errhandler_type_t ompi_errhandler_t ompi_predefined_errhandler_t ompi_mpi_errhandler_null OMPI_ERRHANDLER_CHECK OMPI_ERRHANDLER_RETURN ompi_errhandler_init ompi_errhandler_finalize OMPI_ERRHANDLER_INVOKE ompi_errhandler_invoke ompi_errhandler_request_invoke ompi_errhandler_create ompi_errhandler_is_intrinsic ompi_errhandler_fortran_handler_fn_t OMPI_ERR_INIT_FINALIZE MPI_Errhandler"
SEARCH_HEADER[9]="ompi/errhandler/errhandler_predefined.h ompi_mpi_errors_are_fatal_ ompi_mpi_errors_return_ ompi_mpi_errors_throw_exceptions"
###
SEARCH_HEADER[10]="ompi/file/file.h OMPI_FILE_ISCLOSED OMPI_FILE_HIDDEN ompi_file_t ompi_predefined_file_t ompi_mpi_file_null ompi_file_f_to_c_table ompi_file_init ompi_file_open ompi_file_set_name ompi_file_close ompi_file_finalize ompi_file_invalid MPI_File MPI_FILE_NULL ompi_mpi_cxx_file_errhandler_invoke" # THE LAST ONE WAS FOR THE CXX INTERFACE
SEARCH_HEADER[11]="ompi/group/group.h ompi_group_sporadic_list_t ompi_group_sporadic_data_t ompi_group_strided_data_t ompi_group_bitmap_data_t ompi_group_t ompi_predefined_group_t OMPI_GROUP_ ompi_group_f_to_c_table ompi_mpi_group_null ompi_group_allocate ompi_group_increment_proc_count ompi_group_decrement_proc_count ompi_group_size ompi_group_rank ompi_set_group_rank ompi_group_translate_ranks ompi_group_free ompi_group_get_proc_ptr ompi_group_calc_ ompi_group_peer_lookup ompi_group_div_ceil MPI_Group"
SEARCH_HEADER[12]="ompi/info/info.h MPI_Info ompi_info_t ompi_predefined_info_t ompi_info_f_to_c_table ompi_info_entry_t ompi_mpi_info_null ompi_info_init ompi_info_finalize ompi_info_dup ompi_info_set ompi_info_free ompi_info_get_bool ompi_info_get ompi_info_delete ompi_info_get_valuelen ompi_info_get_nthkey ompi_info_value_to_bool ompi_info_value_to_int ompi_info_is_freed"
###
SEARCH_HEADER[13]="ompi/mca/allocator/allocator.h mca_allocator_base_module_t mca_allocator_base_output mca_allocator_base_component_t"
###
SEARCH_HEADER[14]="ompi/mca/bml/bml.h mca_bml_base_btl_t mca_bml_base_btl_array_t mca_bml_base_btl_array_get_size mca_bml_base_btl_array_set_size mca_bml_base_btl_array_insert mca_bml_base_btl_array_remove mca_bml_base_btl_array_get_index mca_bml_base_btl_array_get_next mca_bml_base_btl_array_find mca_bml_base_endpoint_t mca_bml_base_alloc mca_bml_base_free mca_bml_base_send mca_bml_base_send_status mca_bml_base_sendi mca_bml_base_put mca_bml_base_get mca_bml_base_prepare_src mca_bml_base_prepare_dst mca_bml_base_component_t mca_bml_base_module_t"
###
SEARCH_HEADER[15]="ompi/mca/btl/btl.h mca_btl_base_tag_t MCA_BTL_AM_FRAMEWORK_MASK MCA_BTL_TAG_ MCA_BTL_FLAGS_ MCA_BTL_EXCLUSIVITY_ MCA_BTL_ERROR_FLAGS_FATAL mca_btl_base_segment_t mca_btl_base_descriptor_t MCA_BTL_DES_ mca_btl_base_header_t MCA_BTL_BASE_HEADER_HTON MCA_BTL_BASE_HEADER_NTOH mca_btl_base_component_t mca_btl_base_module_t"
###
SEARCH_HEADER[16]="ompi/mca/coll/coll.h mca_coll_base_component_ mca_coll_base_module_ mca_coll_base_comm_coll_t"
###
SEARCH_HEADER[17]="ompi/mca/dpm/dpm.h OMPI_RML_TAG_ OMPI_CRCP_COORD_BOOKMARK_TAG OMPI_COMM_JOIN_TAG ompi_dpm ompi_dpm_base_component_t"
###
SEARCH_HEADER[18]="ompi/mca/mpool/mpool.h mca_mpool_base_registration_t mca_mpool_base_component_t mca_mpool_base_module_t mca_mpool_base_alloc mca_mpool_base_free mca_mpool_base_tree_node_compare mca_mpool_base_insert mca_mpool_base_remove"
###
SEARCH_HEADER[19]="ompi/mca/pml/pml.h mca_pml_base_send_mode_t OMPI_ANY_TAG OMPI_ANY_SOURCE OMPI_PROC_NULL mca_pml_base_component_t mca_pml_base_module_t MCA_PML_CALL mca_pml"
###
SEARCH_HEADER[20]="ompi/mca/topo/topo.h mca_topo_base_module_ mca_topo_base_component_2_0_0_t mca_topo_base_component_t mca_topo_base_comm_1_0_0_t mca_topo_base_comm_t mca_topo_base_module_t"
###
SEARCH_HEADER[21]="ompi/op/op.h OMPI_OP_FLAGS_ ompi_op_f_to_c_table ompi_op_t ompi_predefined_op_t ompi_op_ddt_map ompi_mpi_op_null ompi_mpi_op_max ompi_mpi_op_min ompi_mpi_op_sum ompi_mpi_op_prod ompi_mpi_op_land ompi_mpi_op_band ompi_mpi_op_lor ompi_mpi_op_bor ompi_mpi_op_lxor ompi_mpi_op_bxor ompi_mpi_op_maxloc ompi_mpi_op_minloc ompi_mpi_op_replace ompi_op_init ompi_op_finalize ompi_op_create_user ompi_op_set_cxx_callback ompi_op_is_intrinsic ompi_op_is_commute ompi_op_is_float_assoc ompi_op_is_valid ompi_op_reduce ompi_3buff_op_reduce ompi_op_fortran_handler_fn_t MPI_Op"
###
SEARCH_HEADER[22]="ompi/proc/proc.h ompi_proc_t ompi_proc_local_proc ompi_proc_init ompi_proc_set_arch ompi_proc_finalize ompi_proc_world ompi_proc_all ompi_proc_self ompi_proc_local ompi_proc_find ompi_proc_pack ompi_proc_unpack ompi_proc_refresh"
###
SEARCH_HEADER[23]="ompi/request/request.h ompi_request_t ompi_request_type_t OMPI_REQUEST_ ompi_request_state_t ompi_mpi_object_t ompi_predefined_request_t OMPI_REQUEST_INIT OMPI_REQUEST_FINI ompi_request_fns_t ompi_request_f_to_c_table ompi_request_waiting ompi_request_completed ompi_request_poll ompi_request_lock ompi_request_cond ompi_request_null ompi_request_empty ompi_status_empty ompi_request_functions ompi_request_init ompi_request_persistent_proc_null_free ompi_request_finalize ompi_request_cancel ompi_request_free ompi_request_test ompi_request_wait ompi_request_wait_completion ompi_request_complete"
###
SEARCH_HEADER[24]="ompi/runtime/ompi_cr.h ompi_cr_init ompi_cr_finalize ompi_cr_coord ompi_cr_output ompi_cr_continue_like_restart"
###
SEARCH_HEADER[25]="ompi/runtime/ompi_module_exchange.h ompi_modex_send ompi_modex_send_string ompi_modex_recv ompi_modex_recv_string"
###
SEARCH_HEADER[26]="ompi/runtime/params.h ompi_mpi_param_check ompi_debug_show_handle_leaks ompi_debug_show_mpi_alloc_mem_leaks ompi_debug_no_free_handles ompi_mpi_show_mca_params ompi_mpi_show_mca_params_file ompi_mpi_paffinity_alone ompi_mpi_keep_peer_hostnames ompi_mpi_abort_print_stack ompi_mpi_abort_delay ompi_mpi_leave_pinned ompi_mpi_leave_pinned_pipeline ompi_have_sparse_group_storage ompi_use_sparse_group_storage ompi_mpi_register_params ompi_show_all_mca_params MPI_PARAM_CHECK"
###
SEARCH_HEADER[27]="ompi/win/win.h OMPI_WIN_FREED OMPI_WIN_INVALID OMPI_WIN_NO_LOCKS OMPI_WIN_ACCESS_EPOCH OMPI_WIN_EXPOSE_EPOCH OMPI_WIN_FENCE OMPI_WIN_POSTED OMPI_WIN_STARTED OMPI_WIN_LOCK_ACCESS ompi_mpi_windows ompi_win_t ompi_predefined_win_t ompi_mpi_win_null ompi_win_init ompi_win_finalize ompi_win_create ompi_win_free ompi_win_set_name ompi_win_get_name ompi_win_group ompi_win_invalid ompi_win_peer_invalid ompi_win_rank ompi_win_allow_locks ompi_win_get_mode ompi_win_set_mode ompi_win_append_mode ompi_win_remove_mode ompi_win_access_epoch ompi_win_exposure_epoch ompi_win_comm_allowed MPI_Win"
SEARCH_HEADER[28]=""

delete_unnessary_header .


echo "PLEASE RUN the following lines in ompi/mpi/c; then run this script again (please header_replacement.sh for add_header)"
echo "# cd ompi/mpi/c"
echo "# for i in *.c ; do grep -q '#include \"ompi/errhandler/errhandler.h\"' $$i || add_header $$i ompi/errhandler/errhandler.h ompi/mpi/c/bindings.h ; done"
echo "# for i in *.c ; do grep -q '#include \"ompi/communicator/communicator.h\"' $$i || add_header $$i ompi/communicator/communicator.h ompi/mpi/c/bindings.h ; done"
echo "# for i in *.c ; do grep -q '#include \"ompi/runtime/params.h\"' $$i || add_header $$i ompi/runtime/params.h ompi/mpi/c/bindings.h ; done"


# Finally erase a header that has been introduced for the STCI replacement
# and is not necessary, if no occurences of #include "prrte..."  reside...
SEARCH_HEADER[0]="rte.h prrte"
SEARCH_HEADER[1]=""

delete_unnessary_header .
