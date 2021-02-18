/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2021 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <ctype.h>

#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif

#include "src/util/argv.h"
#include "src/util/keyval_parse.h"
#include "src/util/os_dirpath.h"
#include "src/util/prte_environ.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/schizo/base/base.h"
#include "schizo_ompi.h"

static int define_cli(prte_cmd_line_t *cli);
static void register_deprecated_cli(prte_list_t *convertors);
static int parse_cli(int argc, int start,
                     char **argv,
                     char *personality,
                     char ***target);
static void parse_proxy_cli(prte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(prte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static int detect_proxy(char **argv);
static int allow_run_as_root(prte_cmd_line_t *cmd_line);
static void job_info(prte_cmd_line_t *cmdline, void *jobinfo);

prte_schizo_base_module_t prte_schizo_ompi_module = {
    .define_cli = define_cli,
    .register_deprecated_cli = register_deprecated_cli,
    .parse_cli = parse_cli,
    .parse_proxy_cli = parse_proxy_cli,
    .parse_env = parse_env,
    .detect_proxy = detect_proxy,
    .allow_run_as_root = allow_run_as_root,
    .job_info = job_info
};

static char *frameworks[] = {
    /* OPAL frameworks */
    "allocator",
    "backtrace",
    "btl",
    "compress",
    "crs",
    "dl",
    "event",
    "hwloc",
    "if",
    "installdirs",
    "memchecker",
    "memcpy",
    "memory",
    "mpool",
    "patcher",
    "pmix",
    "pstat",
    "rcache",
    "reachable",
    "shmem",
    "threads",
    "timer",
    /* OMPI frameworks */
    "mpi", /* global options set in runtime/ompi_mpi_params.c */
    "bml",
    "coll",
    "fbtl",
    "fcoll",
    "fs",
    "hook",
    "io",
    "mtl",
    "op",
    "osc",
    "pml",
    "sharedfp",
    "topo",
    "vprotocol",
    /* OSHMEM frameworks */
    "memheap",
    "scoll",
    "spml",
    "sshmem",
    NULL,
};


/* Cmd-line options common to PRTE master/daemons/tools */
static prte_cmd_line_init_t cmd_line_init[] = {

    /* setup MCA parameters */
    { '\0', "omca", 2, PRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific OMPI MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gomca", 2, PRTE_CMD_LINE_TYPE_STRING,
      "Pass global OMPI MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "tune", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Profile options file list for OMPI applications",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "stream-buffering", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Adjust buffering for stdout/stderr [0 unbuffered] [1 line buffered] [2 fully buffered]",
      PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* mpiexec mandated form launch key parameters */
    { '\0', "initial-errhandler", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Specify the initial error handler that is attached to predefined communicators during the first MPI call.",
      PRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "with-ft", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Specify the type(s) of error handling that the application will use.",
      PRTE_CMD_LINE_OTYPE_LAUNCH },

    /* DVM-specific options */
    /* uri of PMIx publish/lookup server, or at least where to get it */
    { '\0', "ompi-server", 1, PRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRTE_CMD_LINE_OTYPE_DVM },
    /* fwd mpirun port */
    { '\0', "fwd-mpirun-port", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Forward mpirun port to compute node daemons so all will use it",
      PRTE_CMD_LINE_OTYPE_DVM },

    /* Display Commumication Protocol : MPI_Init */
    { '\0', "display-comm", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Display table of communication methods between ranks during MPI_Init",
      PRTE_CMD_LINE_OTYPE_GENERAL },

    /* Display Commumication Protocol : MPI_Finalize */
    { '\0', "display-comm-finalize", 0, PRTE_CMD_LINE_TYPE_BOOL,
      "Display table of communication methods between ranks during MPI_Finalize",
      PRTE_CMD_LINE_OTYPE_GENERAL },


    /* End of list */
    { '\0', NULL, 0, PRTE_CMD_LINE_TYPE_NULL, NULL }
};

static bool checkus(void)
{
    bool takeus = false;
    char *ptr;
    size_t i;
    uint vers;

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prte_schizo_base.personalities) {
        for (i=0; NULL != prte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prte_schizo_base.personalities[i], "ompi")) {
                /* they didn't specify a level, so we will service
                 * them just in case */
                takeus = true;
                break;
            }
            if (0 == strncmp(prte_schizo_base.personalities[i], "ompi", 4)) {
                /* if they specifically requested an ompi level greater
                 * than or equal to us, then we service it */
                ptr = &prte_schizo_base.personalities[i][4];
                vers = strtoul(ptr, NULL, 10);
                if (vers >= 5) {
                    takeus = true;
                }
                break;
            }
        }
    }

    return takeus;
}


static int define_cli(prte_cmd_line_t *cli)
{
    int rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: define_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (!checkus()) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    rc = prte_cmd_line_add(cli, cmd_line_init);
    if (PRTE_SUCCESS != rc){
        return rc;
    }

    /* see if we were given a location where we can get
     * the list of OMPI frameworks */

    return PRTE_SUCCESS;
}

static int parse_deprecated_cli(char *option, char ***argv, int i)
{
    char **pargs, *p2, *modifier;
    int rc = PRTE_SUCCESS;

    pargs = *argv;

    /* --nolocal -> --map-by :nolocal */
    if (0 == strcmp(option, "--nolocal")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "NOLOCAL");
    }
    /* --oversubscribe -> --map-by :OVERSUBSCRIBE
     * --nooversubscribe -> --map-by :NOOVERSUBSCRIBE
     */
    else if (0 == strcmp(option, "--oversubscribe") ||
             0 == strcmp(option, "--nooversubscribe") ) {
        if (0 == strcmp(option, "--nooversubscribe")) {
            prte_show_help("help-schizo-base.txt", "deprecated-inform", true,
                            option, "This is the default behavior so does not need to be specified");
            modifier = "NOOVERSUBSCRIBE";
        } else {
            modifier = "OVERSUBSCRIBE";
        }
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", NULL, modifier);
    }
    /* --use-hwthread-cpus -> --bind-to hwthread */
    else if (0 == strcmp(option, "--use-hwthread-cpus")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "hwthread", NULL);
    }
    /* --cpu-set and --cpu-list -> --map-by pe-list:X
     */
    else if (0 == strcmp(option, "--cpu-set") ||
             0 == strcmp(option, "--cpu-list") ) {
        prte_asprintf(&p2, "PE-LIST=%s", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2);
        free(p2);
    }
    /* --bind-to-core and --bind-to-socket -> --bind-to X */
    else if (0 == strcmp(option, "--bind-to-core")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "core", NULL);
    }
    else if (0 == strcmp(option, "--bind-to-socket")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--bind-to", "socket", NULL);
    }
    /* --bynode -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bynode")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "node", NULL);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prte_schizo_base_convert(argv, i, 0, "--rank-by", "node", NULL);
    }
    /* --bycore -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bycore")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "core", NULL);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prte_schizo_base_convert(argv, i, 0, "--rank-by", "core", NULL);
    }
    /* --byslot -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--byslot")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "slot", NULL);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prte_schizo_base_convert(argv, i, 0, "--rank-by", "slot", NULL);
    }
    /* --cpus-per-proc/rank X -> --map-by :pe=X */
    else if (0 == strcmp(option, "--cpus-per-proc") ||
             0 == strcmp(option, "--cpus-per-rank") ) {
        prte_asprintf(&p2, "pe=%s", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2);
        free(p2);
    }
    /* --npernode X and --npersocket X -> --map-by ppr:X:node/socket */
    else if (0 == strcmp(option, "--npernode")) {
        prte_asprintf(&p2, "ppr:%s:node", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
    }
    else if (0 == strcmp(option, "--pernode")) {
        rc = prte_schizo_base_convert(argv, i, 1, "--map-by", "ppr:1:node", NULL);
    }
    else if (0 == strcmp(option, "--npersocket")) {
        prte_asprintf(&p2, "ppr:%s:socket", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
   }
    /* --ppr X -> --map-by ppr:X */
    else if (0 == strcmp(option, "--ppr")) {
        /* if they didn't specify a complete pattern, then this is an error */
        if (NULL == strchr(pargs[i+1], ':')) {
            prte_show_help("help-schizo-base.txt", "bad-ppr", true, pargs[i+1]);
            return PRTE_ERR_BAD_PARAM;
        }
        prte_asprintf(&p2, "ppr:%s", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
    }
    /* --am[ca] X -> --tune X */
    else if (0 == strcmp(option, "--amca") ||
             0 == strcmp(option, "--am")) {
        rc = prte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL);
    }
    /* --tune X -> aggregate */
    else if (0 == strcmp(option, "--tune")) {
        rc = prte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL);
    }
    /* --rankfile X -> map-by rankfile:file=X */
    else if (0 == strcmp(option, "--rankfile")) {
        prte_asprintf(&p2, "rankfile:file=%s", pargs[i+1]);
        rc = prte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
    }

    return rc;
}

static void register_deprecated_cli(prte_list_t *convertors)
{
    prte_convertor_t *cv;
    char *options[] = {
        "--nolocal",
        "--oversubscribe",
        "--nooversubscribe",
        "--use-hwthread-cpus",
        "--cpu-set",
        "--cpu-list",
        "--bind-to-core",
        "--bind-to-socket",
        "--bynode",
        "--bycore",
        "--byslot",
        "--cpus-per-proc",
        "--cpus-per-rank",
        "--npernode",
        "--pernode",
        "--npersocket",
        "--ppr",
        "--amca",
        "--am",
        "--rankfile",
        NULL
    };

    if (!checkus()) {
        return;
    }

    cv = PRTE_NEW(prte_convertor_t);
    cv->options = prte_argv_copy(options);
    cv->convert = parse_deprecated_cli;
    prte_list_append(convertors, &cv->super);
}

static char *strip_quotes(char *p)
{
    char *pout;

    /* strip any quotes around the args */
    if ('\"' == p[0]) {
        pout = strdup(&p[1]);
    } else {
        pout = strdup(p);
    }
    if ('\"' == pout[strlen(pout)- 1]) {
        pout[strlen(pout)-1] = '\0';
    }
    return pout;

}

static int check_cache_noadd(char ***c1, char ***c2,
                             char *p1, char *p2)
{
    char **cache;
    char **cachevals;
    int k;

    if (NULL == c1 || NULL == c2) {
        return PRTE_SUCCESS;
    }

    cache = *c1;
    cachevals = *c2;

    if (NULL != cache) {
        /* see if we already have these */
        for (k=0; NULL != cache[k]; k++) {
            if (0 == strcmp(cache[k], p1)) {
                /* we do have it - check for same value */
                if (0 != strcmp(cachevals[k], p2)) {
                    /* this is an error */
                    prte_show_help("help-schizo-base.txt",
                                    "duplicate-value", true,
                                    p1, p2, cachevals[k]);
                    return PRTE_ERR_BAD_PARAM;
                }
            }
        }
    }
    return PRTE_SUCCESS;
}

static int check_cache(char ***c1, char ***c2,
                       char *p1, char *p2)
{
    int rc;

    rc = check_cache_noadd(c1, c2, p1, p2);

    if (PRTE_SUCCESS == rc) {
        /* add them to the cache */
        prte_argv_append_nosize(c1, p1);
        prte_argv_append_nosize(c2, p2);
    }
    return rc;
}

static int process_envar(const char *p, char ***cache, char ***cachevals)
{
    char *value, **tmp;
    char *p1, *p2;
    size_t len;
    int k, rc=PRTE_SUCCESS;
    bool found;

    p1 = strdup(p);
    if (NULL != (value = strchr(p1, '='))) {
        /* terminate the name of the param */
        *value = '\0';
        /* step over the equals */
        value++;
        rc = check_cache(cache, cachevals, p1, value);
    } else {
        /* check for a '*' wildcard at the end of the value */
        if ('*' == p1[strlen(p1)-1]) {
            /* search the local environment for all params
             * that start with the string up to the '*' */
            p1[strlen(p1)-1] = '\0';
            len = strlen(p1);
            for (k=0; NULL != environ[k]; k++) {
                if (0 == strncmp(environ[k], p1, len)) {
                    value = strdup(environ[k]);
                    /* find the '=' sign */
                    p2 = strchr(value, '=');
                    *p2 = '\0';
                    ++p2;
                    rc = check_cache(cache, cachevals, value, p2);
                    free(value);
                }
            }
        } else {
            value = getenv(p1);
            if (NULL != value) {
                rc = check_cache(cache, cachevals, p1, value);
            } else {
                found = false;
                if (NULL != cache) {
                    /* see if it is already in the cache */
                    tmp = *cache;
                    for (k=0; NULL != tmp[k]; k++) {
                        if (0 == strncmp(p1, tmp[k], strlen(p1))) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    prte_show_help("help-schizo-base.txt", "env-not-found", true, p1);
                    rc = PRTE_ERR_NOT_FOUND;
                }
            }
        }
    }
    free(p1);
    return rc;
}

/* process params from an env_list - add them to the cache */
static int process_token(char *token, char ***cache, char ***cachevals)
{
    char *ptr, *value;
    int rc;

    if (NULL == (ptr = strchr(token, '='))) {
        value = getenv(token);
        if (NULL == value) {
            return PRTE_ERR_NOT_FOUND;
        }

        /* duplicate the value to silence tainted string coverity issue */
        value = strdup(value);
        if (NULL == value) {
            /* out of memory */
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        if (NULL != (ptr = strchr(value, '='))) {
            *ptr = '\0';
            rc = check_cache(cache, cachevals, value, ptr+1);
        } else {
            rc = check_cache(cache, cachevals, token, value);
        }

        free(value);
    } else {
        *ptr = '\0';
        rc = check_cache(cache, cachevals, token, ptr+1);
        /* NTH: don't bother resetting ptr to = since the string will not be used again */
    }
    return rc;
}

static int process_env_list(const char *env_list,
                            char ***xparams, char ***xvals,
                            char sep)
{
    char** tokens;
    int rc = PRTE_SUCCESS;

    tokens = prte_argv_split(env_list, (int)sep);
    if (NULL == tokens) {
        return PRTE_SUCCESS;
    }

    for (int i = 0 ; NULL != tokens[i] ; ++i) {
        rc = process_token(tokens[i], xparams, xvals);
        if (PRTE_SUCCESS != rc) {
            if (PRTE_ERR_NOT_FOUND == rc) {
                prte_show_help("help-schizo-base.txt", "incorrect-env-list-param",
                                true, tokens[i], env_list);
            }
            break;
        }
    }

    prte_argv_free(tokens);
    return rc;
}

static char *schizo_getline(FILE *fp)
{
    char *ret, *buff;
    char input[2048];

    memset(input, 0, 2048);
    ret = fgets(input, 2048, fp);
    if (NULL != ret) {
       input[strlen(input)-1] = '\0';  /* remove newline */
       buff = strdup(input);
       return buff;
    }

    return NULL;
}

static int process_tune_files(char *filename, char ***dstenv, char sep)
{
    FILE *fp;
    char **tmp, **opts, *line, *param, *p1, *p2;
    int i, n, rc=PRTE_SUCCESS;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;

    tmp = prte_argv_split(filename, sep);
    if (NULL == tmp) {
        return PRTE_SUCCESS;
    }

    /* Iterate through all the files passed in -- it is an ERROR if
     * a given param appears more than once with different values */

    for (i=0; NULL != tmp[i]; i++) {
        fp = fopen(tmp[i], "r");
        if (NULL == fp) {
            prte_show_help("help-schizo-base.txt", "missing-param-file", true, tmp[i]);
            prte_argv_free(tmp);
            prte_argv_free(cache);
            prte_argv_free(cachevals);
            prte_argv_free(xparams);
            prte_argv_free(xvals);
            return PRTE_ERR_NOT_FOUND;
        }
        while (NULL != (line = schizo_getline(fp))) {
            if('\0' == line[0]) continue; /* skip empty lines */
            opts = prte_argv_split_with_empty(line, ' ');
            if (NULL == opts) {
                prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                free(line);
                prte_argv_free(tmp);
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
                fclose(fp);
                return PRTE_ERR_BAD_PARAM;
            }
            for (n=0; NULL != opts[n]; n++) {
                if ('\0' == opts[n][0] || '#' == opts[n][0]) {
                    /* the line is only spaces, or a comment, ignore */
                    break;
                }
                if (0 == strcmp(opts[n], "-x")) {
                    /* the next value must be the envar */
                    if (NULL == opts[n+1]) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    p1 = strip_quotes(opts[n+1]);
                    /* some idiot decided to allow spaces around an "=" sign, which is
                     * a violation of the Posix cmd line syntax. Rather than fighting
                     * the battle to correct their error, try to accommodate it here */
                    if (NULL != opts[n+2] && 0 == strcmp(opts[n+2], "=")) {
                        if (NULL == opts[n+3]) {
                            prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                            free(line);
                            prte_argv_free(tmp);
                            prte_argv_free(opts);
                            prte_argv_free(cache);
                            prte_argv_free(cachevals);
                            prte_argv_free(xparams);
                            prte_argv_free(xvals);
                            fclose(fp);
                            return PRTE_ERR_BAD_PARAM;
                        }
                        p2 = strip_quotes(opts[n+3]);
                        prte_asprintf(&param, "%s=%s", p1, p2);
                        free(p1);
                        free(p2);
                        p1 = param;
                        ++n;  // need an extra step
                    }
                    rc = process_envar(p1, &xparams, &xvals);
                    free(p1);
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    ++n;  // skip over the envar option
                } else if (0 == strcmp(opts[n], "--mca")) {
                    if (NULL == opts[n+1] || NULL == opts[n+2]) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    p1 = strip_quotes(opts[n+1]);
                    p2 = strip_quotes(opts[n+2]);
                    if (0 == strcmp(p1, "mca_base_env_list")) {
                        /* next option must be the list of envars */
                        rc = process_env_list(p2, &xparams, &xvals, ';');
                    } else {
                        /* treat it as an arbitrary MCA param */
                        rc = check_cache(&cache, &cachevals, p1, p2);
                    }
                    free(p1);
                    free(p2);
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    n += 2;  // skip over the MCA option
                } else if (0 == strncmp(opts[n], "mca_base_env_list", strlen("mca_base_env_list"))) {
                    /* find the equal sign */
                    p1 = strchr(opts[n], '=');
                    if (NULL == p1) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        fclose(fp);
                        return PRTE_ERR_BAD_PARAM;
                    }
                    ++p1;
                    rc = process_env_list(p1, &xparams, &xvals, ';');
                    if (PRTE_SUCCESS != rc) {
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                } else {
                    rc = process_token(opts[n], &cache, &cachevals);
                    if (PRTE_SUCCESS != rc) {
                        prte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        fclose(fp);
                        prte_argv_free(tmp);
                        prte_argv_free(opts);
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(xparams);
                        prte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                }
            }
            free(line);
        }
        fclose(fp);
    }

    prte_argv_free(tmp);

    if (NULL != cache) {
        /* add the results into dstenv */
        for (i=0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
        prte_argv_free(cache);
        prte_argv_free(cachevals);
    }

    /* add the -x values */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prte_argv_free(xparams);
        prte_argv_free(xvals);
    }

    return PRTE_SUCCESS;
}

static bool check_generic(char *p1)
{
    int j;

    /* this is a generic MCA designation, so see if the parameter it
     * refers to belongs to a project base or one of our frameworks */
    if (0 == strncmp("opal_", p1, strlen("opal_")) ||
        0 == strncmp("ompi_", p1, strlen("ompi_"))) {
        return true;
    } else if (0 == strcmp(p1, "mca_base_env_list")) {
        return true;
    } else {
        for (j=0; NULL != frameworks[j]; j++) {
            if (0 == strncmp(p1, frameworks[j], strlen(frameworks[j]))) {
                return true;
            }
        }
    }

    return false;
}

static int parse_cli(int argc, int start,
                     char **argv,
                     char *personality,
                     char ***target)
{
    char *p1; int i;
    if (NULL != personality &&
        NULL != strstr(personality, "ompi")) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_cli",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    for (i = 0; i < (argc-start); ++i) {
        if (0 == strcmp("--with-ft", argv[i]) ||
            0 == strcmp("-with-ft", argv[i])) {
            if (NULL == argv[i+1]) {
                /* this is an error */
                return PRTE_ERR_FATAL;
            }
            p1 = strip_quotes(argv[i+1]);
            if( 0 != strcmp("no", p1) &&
                0 != strcmp("false", p1) &&
                0 != strcmp("0", p1)) {
                if (NULL == target) {
                    /* push it into our environment */
                    char *param = NULL;
                    asprintf(&param, "PRTE_MCA_prte_enable_ft");
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                           "%s schizo:ompi:parse_cli pushing %s into environment",
                                           PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, "true", true, &environ);
                    //prte_enable_ft = true;
                    prte_enable_recovery = true;
                    asprintf(&param, "OMPI_MCA_mpi_ft_enable");
                    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                                           "%s schizo:ompi:parse_cli pushing %s into environment",
                                           PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), param);
                    prte_setenv(param, "true", true, &environ);
                }
                else {
                    prte_argv_append_nosize(target, "--prtemca");
                    prte_argv_append_nosize(target, "prte_enable_ft");
                    prte_argv_append_nosize(target, "true");
                    prte_argv_append_nosize(target, "--enable-recovery");
                    prte_argv_append_nosize(target, "--mca");
                    prte_argv_append_nosize(target, "mpi_ft_enable");
                    prte_argv_append_nosize(target, "true");
                }
            }
           free(p1);
        }
    }
    return PRTE_SUCCESS;
}

static int parse_env(prte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline)
{
    char *p1, *p2;
    char *env_set_flag;
    char **cache=NULL, **cachevals=NULL;
    char **xparams=NULL, **xvals=NULL;
    char **envlist = NULL, **envtgt = NULL;
    prte_value_t *pval;
    int i, j, rc;

    prte_output_verbose(1, prte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_env",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* if they are filling out a cmd line, then we don't
     * have anything to contribute */
    if (cmdline) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (!checkus()) {
        return PRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* Begin by examining the environment as the cmd line trumps all */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (NULL != env_set_flag) {
        rc = process_env_list(env_set_flag, &xparams, &xvals, ';');
        if (PRTE_SUCCESS != rc) {
            prte_argv_free(xparams);
            prte_argv_free(xvals);
            return rc;
        }
    }
    /* process the resulting cache into the dstenv */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prte_argv_free(xparams);
        xparams = NULL;
        prte_argv_free(xvals);
        xvals = NULL;
    }

    /* now process any tune file specification - the tune file processor
     * will police itself for duplicate values */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "tune", 0, 0))) {
        p1 = strip_quotes(pval->value.data.string);
        rc = process_tune_files(p1, dstenv, ',');
        free(p1);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }

    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "initial-errhandler", 0, 0))) {
        p1 = strip_quotes(pval->value.data.string);
        rc = check_cache(&cache, &cachevals, "mpi_initial_errhandler", p1);
        free(p1);
        if (PRTE_SUCCESS != rc) {
            prte_argv_free(cache);
            prte_argv_free(cachevals);
            return rc;
        }
    }

    if (prte_cmd_line_is_taken(cmd_line, "display-comm") && prte_cmd_line_is_taken(cmd_line, "display-comm-finalize")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_init,mpi_finalize", true, dstenv);
    }
    else if (prte_cmd_line_is_taken(cmd_line, "display-comm")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_init", true, dstenv);
    }
    else if (prte_cmd_line_is_taken(cmd_line, "display-comm-finalize")) {
        prte_setenv("OMPI_MCA_ompi_display_comm", "mpi_finalize", true, dstenv);
    }

    /* now look for any "--mca" options - note that it is an error
     * for the same MCA param to be given more than once if the
     * values differ */
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "omca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "omca", i, 0);
            p1 = strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "omca", i, 1);
            p2 = strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "gomca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "gomca", i, 0);
            p1 = strip_quotes(pval->value.data.string);
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "gomca", i, 1);
            p2 = strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "mca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "mca", i, 0);
            p1 = strip_quotes(pval->value.data.string);
            /* check if this is one of ours */
            if (!check_generic(p1)) {
                free(p1);
                continue;
            }
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "mca", i, 1);
            p2 = strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(envlist);
                return rc;
            }
        }
    }
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "gmca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prte_cmd_line_get_param(cmd_line, "gmca", i, 0);
            p1 = strip_quotes(pval->value.data.string);
            /* check if this is one of ours */
            if (!check_generic(p1)) {
                free(p1);
                continue;
            }
            /* next value on the list is the value */
            pval = prte_cmd_line_get_param(cmd_line, "gmca", i, 1);
            p2 = strip_quotes(pval->value.data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                prte_argv_free(envlist);
                return rc;
            }
        }
    }

    /* if we got any env lists, process them here */
    if (NULL != envlist) {
        for (i=0; NULL != envlist[i]; i++) {
            envtgt = prte_argv_split(envlist[i], ';');
            for (j=0; NULL != envtgt[j]; j++) {
                if (NULL == (p2 = strchr(envtgt[j], '='))) {
                    p1 = getenv(envtgt[j]);
                    if (NULL == p1) {
                        continue;
                    }
                    p1 = strdup(p1);
                    if (NULL != (p2 = strchr(p1, '='))) {
                        *p2 = '\0';
                        rc = check_cache(&xparams, &xvals, p1, p2 + 1);
                    } else {
                        rc = check_cache(&xparams, &xvals, envtgt[j], p1);
                    }
                    free(p1);
                    if (PRTE_SUCCESS != rc) {
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(envtgt);
                        prte_argv_free(envlist);
                        return rc;
                    }
                } else {
                    *p2 = '\0';
                    rc = check_cache(&xparams, &xvals, envtgt[j], p2 + 1);
                    if (PRTE_SUCCESS != rc) {
                        prte_argv_free(cache);
                        prte_argv_free(cachevals);
                        prte_argv_free(envtgt);
                        prte_argv_free(envlist);
                        return rc;
                    }
                }
            }
            prte_argv_free(envtgt);
        }
    }
    prte_argv_free(envlist);

    /* now look for -x options - not allowed to conflict with a -mca option */
    if (0 < (j = prte_cmd_line_get_ninsts(cmd_line, "x"))) {
        for (i = 0; i < j; ++i) {
            /* the value is the envar */
            pval = prte_cmd_line_get_param(cmd_line, "x", i, 0);
            p1 = strip_quotes(pval->value.data.string);
            /* if there is an '=' in it, then they are setting a value */
            if (NULL != (p2 = strchr(p1, '='))) {
                *p2 = '\0';
                ++p2;
            } else {
                p2 = getenv(p1);
                if (NULL == p2) {
                    free(p1);
                    continue;
                }
            }
            /* not allowed to duplicate anything from an MCA param on the cmd line */
            rc = check_cache_noadd(&cache, &cachevals, p1, p2);
            if (PRTE_SUCCESS != rc) {
                prte_argv_free(cache);
                prte_argv_free(cachevals);
                free(p1);
                prte_argv_free(xparams);
                prte_argv_free(xvals);
                return rc;
            }
            /* cache this for later inclusion */
            prte_argv_append_nosize(&xparams, p1);
            prte_argv_append_nosize(&xvals, p2);
            free(p1);
        }
    }

    /* process the resulting cache into the dstenv */
    if (NULL != cache) {
        for (i=0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
    }
    prte_argv_free(cache);
    prte_argv_free(cachevals);

    /* add the -x values */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prte_argv_free(xparams);
        prte_argv_free(xvals);
    }

    return PRTE_SUCCESS;
}

static int detect_proxy(char **argv)
{
    /* if the basename of the cmd was "mpirun" or "mpiexec",
     * we default to us */
    if (prte_schizo_base.test_proxy_launch ||
        0 == strcmp(prte_tool_basename, "mpirun") ||
        0 == strcmp(prte_tool_basename, "mpiexec") ||
        0 == strcmp(prte_tool_basename, "oshrun")) {
        /* add us to the personalities */
        prte_argv_append_unique_nosize(&prte_schizo_base.personalities, "ompi5");
        if (0 == strcmp(prte_tool_basename, "oshrun")) {
            /* add oshmem to the personalities */
            prte_argv_append_unique_nosize(&prte_schizo_base.personalities, "oshmem");
        }
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

static int allow_run_as_root(prte_cmd_line_t *cmd_line)
{
    /* we always run last */
    char *r1, *r2;

    if (prte_cmd_line_is_taken(cmd_line, "allow-run-as-root")) {
        return PRTE_SUCCESS;
    }

    if (NULL != (r1 = getenv("OMPI_ALLOW_RUN_AS_ROOT")) &&
        NULL != (r2 = getenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return PRTE_SUCCESS;
        }
    }

    return PRTE_ERR_TAKE_NEXT_OPTION;
}

static void job_info(prte_cmd_line_t *cmdline, void *jobinfo)
{
    prte_value_t *pval;
    uint16_t u16;
    pmix_status_t rc;

    if (NULL != (pval = prte_cmd_line_get_param(cmdline, "stream-buffering", 0, 0))) {
        u16 = pval->value.data.integer;
        if (0 != u16 && 1 != u16 && 2 != u16) {
            /* bad value */
            prte_show_help("help-schizo-base.txt", "bad-stream-buffering-value", true, pval->value.data.integer);
            return;
        }
        PMIX_INFO_LIST_ADD(rc, jobinfo, "OMPI_STREAM_BUFFERING", &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
}

static void parse_proxy_cli(prte_cmd_line_t *cmd_line,
                            char ***argv)
{
    prte_value_t *pval;

    /* we need to convert some legacy ORTE cmd line options to their
     * PRTE equivalent when launching as a proxy for mpirun */
    if (NULL != (pval = prte_cmd_line_get_param(cmd_line, "orte_tmpdir_base", 0, 0))) {
        prte_argv_append_nosize(argv, "--prtemca");
        prte_argv_append_nosize(argv, "prte_tmpdir_base");
        prte_argv_append_nosize(argv, pval->value.data.string);
    }
}
