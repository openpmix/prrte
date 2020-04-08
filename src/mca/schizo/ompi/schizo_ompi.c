/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
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
#include "src/util/prrte_environ.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/schizo/base/base.h"
#include "schizo_ompi.h"

static int define_cli(prrte_cmd_line_t *cli);
static void register_deprecated_cli(prrte_list_t *convertors);
static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv);
static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline);
static int detect_proxy(char **argv);
static int allow_run_as_root(prrte_cmd_line_t *cmd_line);
static void job_info(prrte_cmd_line_t *cmdline, prrte_list_t *jobinfo);

prrte_schizo_base_module_t prrte_schizo_ompi_module = {
    .define_cli = define_cli,
    .register_deprecated_cli = register_deprecated_cli,
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
    "timer",
    /* OMPI frameworks */
    "bml",
    "coll",
    "crcp",
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


/* Cmd-line options common to PRRTE master/daemons/tools */
static prrte_cmd_line_init_t cmd_line_init[] = {

    /* setup MCA parameters */
    { '\0', "omca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass context-specific OMPI MCA parameters; they are considered global if --gmca is not used and only one context is specified (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "gomca", 2, PRRTE_CMD_LINE_TYPE_STRING,
      "Pass global OMPI MCA parameters that are applicable to all contexts (arg0 is the parameter name; arg1 is the parameter value)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "tune", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Profile options file list for OMPI applications",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { '\0', "stream-buffering", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Adjust buffering for stdout/stderr [0 unbuffered] [1 line buffered] [2 fully buffered]",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },


    /* DVM-specific options */
    /* uri of PMIx publish/lookup server, or at least where to get it */
    { '\0', "ompi-server", 1, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },
    /* fwd mpirun port */
    { '\0', "fwd-mpirun-port", 0, PRRTE_CMD_LINE_TYPE_BOOL,
      "Forward mpirun port to compute node daemons so all will use it",
      PRRTE_CMD_LINE_OTYPE_DVM },


    /* End of list */
    { '\0', NULL, 0, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static bool checkus(void)
{
    bool takeus = false;
    char *ptr;
    size_t i;
    uint vers;

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prrte_schizo_base.personalities) {
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "ompi")) {
                /* they didn't specify a level, so we will service
                 * them just in case */
                takeus = true;
                break;
            }
            if (0 == strncmp(prrte_schizo_base.personalities[i], "ompi", 4)) {
                /* if they specifically requested an ompi level greater
                 * than or equal to us, then we service it */
                ptr = &prrte_schizo_base.personalities[i][4];
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


static int define_cli(prrte_cmd_line_t *cli)
{
    int rc;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: define_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (!checkus()) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    rc = prrte_cmd_line_add(cli, cmd_line_init);
    if (PRRTE_SUCCESS != rc){
        return rc;
    }

    /* see if we were given a location where we can get
     * the list of OMPI frameworks */

    return PRRTE_SUCCESS;
}

static int parse_deprecated_cli(char *option, char ***argv, int i)
{
    char **pargs, *p2, *modifier;
    int rc = PRRTE_SUCCESS;

    pargs = *argv;

    /* --nolocal -> --map-by :nolocal */
    if (0 == strcmp(option, "--nolocal")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, "NOLOCAL");
    }
    /* --oversubscribe -> --map-by :OVERSUBSCRIBE
     * --nooversubscribe -> --map-by :NOOVERSUBSCRIBE
     */
    else if (0 == strcmp(option, "--oversubscribe") ||
             0 == strcmp(option, "--nooversubscribe") ) {
        if (0 == strcmp(option, "--nooversubscribe")) {
            prrte_show_help("help-schizo-base.txt", "deprecated-inform", true,
                            option, "This is the default behavior so does not need to be specified");
            modifier = "NOOVERSUBSCRIBE";
        } else {
            modifier = "OVERSUBSCRIBE";
        }
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", NULL, modifier);
    }
    /* --use-hwthread-cpus -> --bind-to hwthread */
    else if (0 == strcmp(option, "--use-hwthread-cpus")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--bind-to", "hwthread", NULL);
    }
    /* --cpu-set and --cpu-list -> --map-by pe-list:X
     */
    else if (0 == strcmp(option, "--cpu-set") ||
             0 == strcmp(option, "--cpu-list") ) {
        prrte_asprintf(&p2, "PE-LIST=%s", pargs[i+1]);
        rc = prrte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2);
        free(p2);
    }
    /* --bind-to-core and --bind-to-socket -> --bind-to X */
    else if (0 == strcmp(option, "--bind-to-core")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--bind-to", "core", NULL);
    }
    else if (0 == strcmp(option, "--bind-to-socket")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--bind-to", "socket", NULL);
    }
    /* --bynode -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bynode")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", "node", NULL);
        if (PRRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prrte_schizo_base_convert(argv, i, 0, "--rank-by", "node", NULL);
    }
    /* --bycore -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--bycore")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", "core", NULL);
        if (PRRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prrte_schizo_base_convert(argv, i, 0, "--rank-by", "core", NULL);
    }
    /* --byslot -> "--map-by X --rank-by X" */
    else if (0 == strcmp(option, "--byslot")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", "slot", NULL);
        if (PRRTE_SUCCESS != rc) {
            return rc;
        }
        // paired with rank-by - note that we would have already removed the
        // ith location where the option was stored, so don't do it again
        rc = prrte_schizo_base_convert(argv, i, 0, "--rank-by", "slot", NULL);
    }
    /* --cpus-per-proc/rank X -> --map-by :pe=X */
    else if (0 == strcmp(option, "--cpus-per-proc") ||
             0 == strcmp(option, "--cpus-per-rank") ) {
        prrte_asprintf(&p2, "pe=%s", pargs[i+1]);
        rc = prrte_schizo_base_convert(argv, i, 2, "--map-by", NULL, p2);
        free(p2);
    }
    /* --npernode X and --npersocket X -> --map-by ppr:X:node/socket */
    else if (0 == strcmp(option, "--npernode")) {
        prrte_asprintf(&p2, "ppr:%s:node", pargs[i+1]);
        rc = prrte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
    }
    else if (0 == strcmp(option, "--pernode")) {
        rc = prrte_schizo_base_convert(argv, i, 1, "--map-by", "ppr:1:node", NULL);
    }
    else if (0 == strcmp(option, "--npersocket")) {
        prrte_asprintf(&p2, "ppr:%s:socket", pargs[i+1]);
        rc = prrte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
   }
    /* --ppr X -> --map-by ppr:X */
    else if (0 == strcmp(option, "--ppr")) {
        /* if they didn't specify a complete pattern, then this is an error */
        if (NULL == strchr(pargs[i+1], ':')) {
            prrte_show_help("help-schizo-base.txt", "bad-ppr", true, pargs[i+1]);
            return PRRTE_ERR_BAD_PARAM;
        }
        prrte_asprintf(&p2, "ppr:%s", pargs[i+1]);
        rc = prrte_schizo_base_convert(argv, i, 2, "--map-by", p2, NULL);
        free(p2);
    }
    /* --am[ca] X -> --tune X */
    else if (0 == strcmp(option, "--amca") ||
             0 == strcmp(option, "--am")) {
        rc = prrte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL);
    }
    /* --tune X -> aggregate */
    else if (0 == strcmp(option, "--tune")) {
        rc = prrte_schizo_base_convert(argv, i, 2, "--tune", NULL, NULL);
    }

    return rc;
}

static void register_deprecated_cli(prrte_list_t *convertors)
{
    prrte_convertor_t *cv;
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
        NULL
    };

    if (!checkus()) {
        return;
    }

    cv = PRRTE_NEW(prrte_convertor_t);
    cv->options = prrte_argv_copy(options);
    cv->convert = parse_deprecated_cli;
    prrte_list_append(convertors, &cv->super);
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
        return PRRTE_SUCCESS;
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
                    prrte_show_help("help-schizo-base.txt",
                                    "duplicate-value", true,
                                    p1, p2, cachevals[k]);
                    return PRRTE_ERR_BAD_PARAM;
                }
            }
        }
    }
    return PRRTE_SUCCESS;
}

static int check_cache(char ***c1, char ***c2,
                       char *p1, char *p2)
{
    int rc;

    rc = check_cache_noadd(c1, c2, p1, p2);

    if (PRRTE_SUCCESS == rc) {
        /* add them to the cache */
        prrte_argv_append_nosize(c1, p1);
        prrte_argv_append_nosize(c2, p2);
    }
    return rc;
}

static int process_envar(const char *p, char ***cache, char ***cachevals)
{
    char *value, **tmp;
    char *p1, *p2;
    size_t len;
    int k, rc=PRRTE_SUCCESS;
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
                    prrte_show_help("help-schizo-base.txt", "env-not-found", true, p1);
                    rc = PRRTE_ERR_NOT_FOUND;
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
            return PRRTE_ERR_NOT_FOUND;
        }

        /* duplicate the value to silence tainted string coverity issue */
        value = strdup(value);
        if (NULL == value) {
            /* out of memory */
            return PRRTE_ERR_OUT_OF_RESOURCE;
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
    int rc = PRRTE_SUCCESS;

    tokens = prrte_argv_split(env_list, (int)sep);
    if (NULL == tokens) {
        return PRRTE_SUCCESS;
    }

    for (int i = 0 ; NULL != tokens[i] ; ++i) {
        rc = process_token(tokens[i], xparams, xvals);
        if (PRRTE_SUCCESS != rc) {
            if (PRRTE_ERR_NOT_FOUND == rc) {
                prrte_show_help("help-schizo-base.txt", "incorrect-env-list-param",
                                true, tokens[i], env_list);
            }
            break;
        }
    }

    prrte_argv_free(tokens);
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
    int i, n, rc=PRRTE_SUCCESS;
    char **cache = NULL, **cachevals = NULL;
    char **xparams = NULL, **xvals = NULL;

    tmp = prrte_argv_split(filename, sep);
    if (NULL == tmp) {
        return PRRTE_SUCCESS;
    }

    /* Iterate through all the files passed in -- it is an ERROR if
     * a given param appears more than once with different values */

    for (i=0; NULL != tmp[i]; i++) {
        fp = fopen(tmp[i], "r");
        if (NULL == fp) {
            prrte_show_help("help-schizo-base.txt", "missing-param-file", true, tmp[i]);
            prrte_argv_free(tmp);
            prrte_argv_free(cache);
            prrte_argv_free(cachevals);
            prrte_argv_free(xparams);
            prrte_argv_free(xvals);
            return PRRTE_ERR_NOT_FOUND;
        }
        while (NULL != (line = schizo_getline(fp))) {
            opts = prrte_argv_split(line, ' ');
            if (NULL == opts) {
                prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                free(line);
                prrte_argv_free(tmp);
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                prrte_argv_free(xparams);
                prrte_argv_free(xvals);
                fclose(fp);
                return PRRTE_ERR_BAD_PARAM;
            }
            for (n=0; NULL != opts[n]; n++) {
                if (0 == strcmp(opts[n], "-x")) {
                    /* the next value must be the envar */
                    if (NULL == opts[n+1]) {
                        prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        fclose(fp);
                        return PRRTE_ERR_BAD_PARAM;
                    }
                    p1 = strip_quotes(opts[n+1]);
                    /* some idiot decided to allow spaces around an "=" sign, which is
                     * a violation of the Posix cmd line syntax. Rather than fighting
                     * the battle to correct their error, try to accommodate it here */
                    if (NULL != opts[n+2] && 0 == strcmp(opts[n+2], "=")) {
                        if (NULL == opts[n+3]) {
                            prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                            free(line);
                            prrte_argv_free(tmp);
                            prrte_argv_free(opts);
                            prrte_argv_free(cache);
                            prrte_argv_free(cachevals);
                            prrte_argv_free(xparams);
                            prrte_argv_free(xvals);
                            fclose(fp);
                            return PRRTE_ERR_BAD_PARAM;
                        }
                        p2 = strip_quotes(opts[n+3]);
                        prrte_asprintf(&param, "%s=%s", p1, p2);
                        free(p1);
                        free(p2);
                        p1 = param;
                        ++n;  // need an extra step
                    }
                    rc = process_envar(p1, &xparams, &xvals);
                    free(p1);
                    if (PRRTE_SUCCESS != rc) {
                        fclose(fp);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    ++n;  // skip over the envar option
                } else if (0 == strcmp(opts[n], "--mca")) {
                    if (NULL == opts[n+1] || NULL == opts[n+2]) {
                        prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        fclose(fp);
                        return PRRTE_ERR_BAD_PARAM;
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
                    if (PRRTE_SUCCESS != rc) {
                        fclose(fp);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                    n += 2;  // skip over the MCA option
                } else if (0 == strncmp(opts[n], "mca_base_env_list", strlen("mca_base_env_list"))) {
                    /* find the equal sign */
                    p1 = strchr(opts[n], '=');
                    if (NULL == p1) {
                        prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        free(line);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        fclose(fp);
                        return PRRTE_ERR_BAD_PARAM;
                    }
                    ++p1;
                    rc = process_env_list(p1, &xparams, &xvals, ';');
                    if (PRRTE_SUCCESS != rc) {
                        fclose(fp);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                } else {
                    rc = process_token(opts[n], &cache, &cachevals);
                    if (PRRTE_SUCCESS != rc) {
                        prrte_show_help("help-schizo-base.txt", "bad-param-line", true, tmp[i], line);
                        fclose(fp);
                        prrte_argv_free(tmp);
                        prrte_argv_free(opts);
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(xparams);
                        prrte_argv_free(xvals);
                        free(line);
                        return rc;
                    }
                }
            }
            free(line);
        }
        fclose(fp);
    }

    prrte_argv_free(tmp);

    if (NULL != cache) {
        /* add the results into dstenv */
        for (n=0; NULL != cache[n]; n++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prrte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prrte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prrte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
        prrte_argv_free(cache);
        prrte_argv_free(cachevals);
    }

    /* add the -x values */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prrte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prrte_argv_free(xparams);
        prrte_argv_free(xvals);
    }

    return PRRTE_SUCCESS;
}

static bool check_generic(char *p1)
{
    int j;

    /* this is a generic MCA designation, so see if the parameter it
     * refers to belongs to a project base or one of our frameworks */
    if (0 == strncmp("opal_", p1, strlen("opal_")) ||
        0 == strncmp("orte_", p1, strlen("orte_")) ||
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

static int parse_env(prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv,
                     bool cmdline)
{
    char *p1, *p2;
    char *env_set_flag;
    char **cache=NULL, **cachevals=NULL;
    char **xparams=NULL, **xvals=NULL;
    char **envlist = NULL, **envtgt = NULL;
    prrte_value_t *pval;
    int i, j, rc;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:ompi: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if they are filling out a cmd line, then we don't
     * have anything to contribute */
    if (cmdline) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (!checkus()) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    /* Begin by examining the environment as the cmd line trumps all */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (NULL != env_set_flag) {
        rc = process_env_list(env_set_flag, &xparams, &xvals, ';');
        if (PRRTE_SUCCESS != rc) {
            prrte_argv_free(xparams);
            prrte_argv_free(xvals);
            return rc;
        }
    }
    /* process the resulting cache into the dstenv */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prrte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prrte_argv_free(xparams);
        xparams = NULL;
        prrte_argv_free(xvals);
        xvals = NULL;
    }

    /* now process any tune file specification - the tune file processor
     * will police itself for duplicate values */
    if (NULL != (pval = prrte_cmd_line_get_param(cmd_line, "tune", 0, 0))) {
        p1 = strip_quotes(pval->data.string);
        rc = process_tune_files(p1, dstenv, ',');
        free(p1);
        if (PRRTE_SUCCESS != rc) {
            return rc;
        }
    }

    /* now look for any "--mca" options - note that it is an error
     * for the same MCA param to be given more than once if the
     * values differ */
    if (0 < (j = prrte_cmd_line_get_ninsts(cmd_line, "omca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prrte_cmd_line_get_param(cmd_line, "omca", i, 0);
            p1 = strip_quotes(pval->data.string);
            /* next value on the list is the value */
            pval = prrte_cmd_line_get_param(cmd_line, "omca", i, 1);
            p2 = strip_quotes(pval->data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prrte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRRTE_SUCCESS != rc) {
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(cmd_line, "gomca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prrte_cmd_line_get_param(cmd_line, "gomca", i, 0);
            p1 = strip_quotes(pval->data.string);
            /* next value on the list is the value */
            pval = prrte_cmd_line_get_param(cmd_line, "gomca", i, 1);
            p2 = strip_quotes(pval->data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prrte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRRTE_SUCCESS != rc) {
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                return rc;
            }
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(cmd_line, "mca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prrte_cmd_line_get_param(cmd_line, "mca", i, 0);
            p1 = strip_quotes(pval->data.string);
            /* check if this is one of ours */
            if (!check_generic(p1)) {
                free(p1);
                continue;
            }
            /* next value on the list is the value */
            pval = prrte_cmd_line_get_param(cmd_line, "mca", i, 1);
            p2 = strip_quotes(pval->data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prrte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRRTE_SUCCESS != rc) {
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                prrte_argv_free(envlist);
                return rc;
            }
        }
    }
    if (0 < (j = prrte_cmd_line_get_ninsts(cmd_line, "gmca"))) {
        for (i = 0; i < j; ++i) {
            /* the first value on the list is the name of the param */
            pval = prrte_cmd_line_get_param(cmd_line, "gmca", i, 0);
            p1 = strip_quotes(pval->data.string);
            /* check if this is one of ours */
            if (!check_generic(p1)) {
                free(p1);
                continue;
            }
            /* next value on the list is the value */
            pval = prrte_cmd_line_get_param(cmd_line, "gmca", i, 1);
            p2 = strip_quotes(pval->data.string);
            /* treat mca_base_env_list as a special case */
            if (0 == strcmp(p1, "mca_base_env_list")) {
                prrte_argv_append_nosize(&envlist, p2);
                free(p1);
                free(p2);
                continue;
            }
            rc = check_cache(&cache, &cachevals, p1, p2);
            free(p1);
            free(p2);
            if (PRRTE_SUCCESS != rc) {
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                prrte_argv_free(envlist);
                return rc;
            }
        }
    }

    /* if we got any env lists, process them here */
    if (NULL != envlist) {
        for (i=0; NULL != envlist[i]; i++) {
            envtgt = prrte_argv_split(envlist[i], ';');
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
                    if (PRRTE_SUCCESS != rc) {
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(envtgt);
                        prrte_argv_free(envlist);
                        return rc;
                    }
                } else {
                    *p2 = '\0';
                    rc = check_cache(&xparams, &xvals, envtgt[j], p2 + 1);
                    if (PRRTE_SUCCESS != rc) {
                        prrte_argv_free(cache);
                        prrte_argv_free(cachevals);
                        prrte_argv_free(envtgt);
                        prrte_argv_free(envlist);
                        return rc;
                    }
                }
            }
            prrte_argv_free(envtgt);
        }
    }
    prrte_argv_free(envlist);

    /* now look for -x options - not allowed to conflict with a -mca option */
    if (0 < (j = prrte_cmd_line_get_ninsts(cmd_line, "x"))) {
        for (i = 0; i < j; ++i) {
            /* the value is the envar */
            pval = prrte_cmd_line_get_param(cmd_line, "x", i, 0);
            p1 = strip_quotes(pval->data.string);
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
            if (PRRTE_SUCCESS != rc) {
                prrte_argv_free(cache);
                prrte_argv_free(cachevals);
                free(p1);
                prrte_argv_free(xparams);
                prrte_argv_free(xvals);
                return rc;
            }
            /* cache this for later inclusion */
            prrte_argv_append_nosize(&xparams, p1);
            prrte_argv_append_nosize(&xvals, p2);
            free(p1);
        }
    }

    /* process the resulting cache into the dstenv */
    if (NULL != cache) {
        for (i=0; NULL != cache[i]; i++) {
            if (0 != strncmp(cache[i], "OMPI_MCA_", strlen("OMPI_MCA_"))) {
                prrte_asprintf(&p1, "OMPI_MCA_%s", cache[i]);
                prrte_setenv(p1, cachevals[i], true, dstenv);
                free(p1);
            } else {
                prrte_setenv(cache[i], cachevals[i], true, dstenv);
            }
        }
    }
    prrte_argv_free(cache);
    prrte_argv_free(cachevals);

    /* add the -x values */
    if (NULL != xparams) {
        for (i=0; NULL != xparams[i]; i++) {
            prrte_setenv(xparams[i], xvals[i], true, dstenv);
        }
        prrte_argv_free(xparams);
        prrte_argv_free(xvals);
    }

    return PRRTE_SUCCESS;
}

static int detect_proxy(char **argv)
{
    /* if the basename of the cmd was "mpirun" or "mpiexec",
     * we default to us */
    if (prrte_schizo_base.test_proxy_launch ||
        0 == strcmp(prrte_tool_basename, "mpirun") ||
        0 == strcmp(prrte_tool_basename, "mpiexec") ||
        0 == strcmp(prrte_tool_basename, "oshrun")) {
        /* add us to the personalities */
        prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "ompi5");
        if (0 == strcmp(prrte_tool_basename, "oshrun")) {
            /* add oshmem to the personalities */
            prrte_argv_append_unique_nosize(&prrte_schizo_base.personalities, "oshmem");
        }
        return PRRTE_SUCCESS;
    }

    return PRRTE_ERR_TAKE_NEXT_OPTION;
}

static int allow_run_as_root(prrte_cmd_line_t *cmd_line)
{
    /* we always run last */
    char *r1, *r2;

    if (prrte_cmd_line_is_taken(cmd_line, "allow_run_as_root")) {
        return PRRTE_SUCCESS;
    }

    if (NULL != (r1 = getenv("OMPI_ALLOW_RUN_AS_ROOT")) &&
        NULL != (r2 = getenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM"))) {
        if (0 == strcmp(r1, "1") && 0 == strcmp(r2, "1")) {
            return PRRTE_SUCCESS;
        }
    }

    return PRRTE_ERR_TAKE_NEXT_OPTION;
}

static void job_info(prrte_cmd_line_t *cmdline, prrte_list_t *jobinfo)
{
    prrte_ds_info_t *ds;
    prrte_value_t *pval;
    uint16_t u16;

    if (NULL != (pval = prrte_cmd_line_get_param(cmdline, "stream-buffering", 0, 0))) {
        u16 = pval->data.integer;
        if (0 != u16 && 1 != u16 && 2 != u16) {
            /* bad value */
            prrte_show_help("help-schizo-base.txt", "bad-stream-buffering-value", true, pval->data.integer);
            return;
        }
        ds = PRRTE_NEW(prrte_ds_info_t);
        PMIX_INFO_CREATE(ds->info, 1);
        PMIX_INFO_LOAD(ds->info, "OMPI_STREAM_BUFFERING", &u16, PMIX_UINT16);
        prrte_list_append(jobinfo, &ds->super);
    }
}

static void parse_proxy_cli(prrte_cmd_line_t *cmd_line,
                            char ***argv)
{
    prrte_value_t *pval;

    /* we need to convert some legacy ORTE cmd line options to their
     * PRRTE equivalent when launching as a proxy for mpirun */
    if (NULL != (pval = prrte_cmd_line_get_param(cmd_line, "orte_tmpdir_base", 0, 0))) {
        prrte_argv_append_nosize(argv, "--prtemca");
        prrte_argv_append_nosize(argv, "prrte_tmpdir_base");
        prrte_argv_append_nosize(argv, pval->data.string);
    }
}
