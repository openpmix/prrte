/*
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

#include "src/util/prte_bootstrap.h"

/*
 * Local functions
 */
static pmix_status_t regex_extract_nodes(char *regexp, char ***names);
static pmix_status_t regex_parse_value_ranges(char *base, char *ranges, int num_digits,
                                              char *suffix, char ***names);
static pmix_status_t regex_parse_value_range(char *base, char *range, int num_digits,
                                             char *suffix, char ***names);
static pmix_status_t read_file(char *regexp, char ***names);

static bool parse_bool(const char *value)
{
    if (NULL == value) {
        return false;
    }
    if (0 == strcasecmp(value, "true") || 0 == strcasecmp(value, "yes")
        || 0 == strcasecmp(value, "on") || 0 == strcmp(value, "1")) {
        return true;
    }
    return false;
}

/* return a normalized (short-form unless FQDN is kept, or an IP address)
 * malloc'd copy of a host name for comparison */
static char *normalize_host(const char *name, bool keep_fqdn)
{
    char *s = strdup(name);
    char *dot;

    if (NULL == s) {
        return NULL;
    }
    if (!keep_fqdn && !pmix_net_isaddr(name)) {
        dot = strchr(s, '.');
        if (NULL != dot) {
            *dot = '\0';
        }
    }
    return s;
}

static bool host_match(const char *a, const char *b, bool keep_fqdn)
{
    char *na, *nb;
    bool match = false;

    if (NULL == a || NULL == b) {
        return false;
    }
    na = normalize_host(a, keep_fqdn);
    nb = normalize_host(b, keep_fqdn);
    if (NULL != na && NULL != nb) {
        match = (0 == strcasecmp(na, nb));
    }
    if (NULL != na) {
        free(na);
    }
    if (NULL != nb) {
        free(nb);
    }
    return match;
}

/* does the candidate name refer to the local node? */
static bool is_local(prte_bootstrap_config_t *cfg, const char *candidate)
{
    int i;

    if (NULL != prte_process_info.nodename
        && host_match(candidate, prte_process_info.nodename, cfg->keep_fqdn)) {
        return true;
    }
    if (NULL != prte_process_info.aliases) {
        for (i = 0; NULL != prte_process_info.aliases[i]; i++) {
            if (host_match(candidate, prte_process_info.aliases[i], cfg->keep_fqdn)) {
                return true;
            }
        }
    }
    return false;
}

int prte_bootstrap_parse(prte_bootstrap_config_t *cfg)
{
    char *path, *line, *ptr;
    FILE *fp;
    char *dvmnodes = NULL;
    int rc = PRTE_ERR_SILENT;
    pmix_status_t ret;

    /* set the defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->cluster = strdup("cluster");
    cfg->port = 7817;
    cfg->ip_version = 4;
    cfg->radix = 64;
    cfg->connect_max_time = 30;
    cfg->retry_max_delay = 5;

    /* see if we can open a configuration file */
    path = pmix_os_path(false, prte_install_dirs.sysconfdir, "prte.conf", NULL);
    fp = fopen(path, "r");
    if (NULL == fp) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-not-found", true,
                       prte_process_info.nodename, path);
        free(path);
        return PRTE_ERR_SILENT;
    }

    while (NULL != (line = pmix_getline(fp))) {
        /* ignore if line is empty or comment */
        if (0 == strlen(line) || '#' == line[0]) {
            free(line);
            continue;
        }
        /* split on the '=' sign */
        if (NULL == (ptr = strchr(line, '='))) {
            pmix_show_help("help-prte-runtime.txt", "bootstrap-bad-entry", true,
                           prte_process_info.nodename, path, line);
            free(line);
            fclose(fp);
            goto cleanup;
        }
        *ptr = '\0';
        if (0 == strlen(line)) { // missing the field name
            *ptr = '=';
            pmix_show_help("help-prte-runtime.txt", "bootstrap-missing-field-name", true,
                           prte_process_info.nodename, path, ptr);
            free(line);
            fclose(fp);
            goto cleanup;
        }
        ++ptr;
        if ('\0' == *ptr) { // missing the value
            pmix_show_help("help-prte-runtime.txt", "bootstrap-missing-value", true,
                           prte_process_info.nodename, path, line);
            free(line);
            fclose(fp);
            goto cleanup;
        }

        /* identify and cache the option */
        if (0 == strcmp(line, "ClusterName")) {
            free(cfg->cluster);
            cfg->cluster = strdup(ptr);

        } else if (0 == strcmp(line, "DVMControllerHost")) {
            if (NULL != cfg->ctrlhost) {
                free(cfg->ctrlhost);
            }
            cfg->ctrlhost = strdup(ptr);

        } else if (0 == strcmp(line, "DVMPort")) {
            cfg->port = strtoul(ptr, NULL, 10);

        } else if (0 == strcmp(line, "DVMIPVersion")) {
            cfg->ip_version = (int) strtol(ptr, NULL, 10);

        } else if (0 == strcmp(line, "DVMRadix")) {
            cfg->radix = (int) strtol(ptr, NULL, 10);

        } else if (0 == strcmp(line, "DVMConnectMaxTime")) {
            cfg->connect_max_time = strtoul(ptr, NULL, 10);

        } else if (0 == strcmp(line, "DVMNodes")) {
            if (NULL != dvmnodes) {
                free(dvmnodes);
            }
            dvmnodes = strdup(ptr);

        } else if (0 == strcmp(line, "KeepFQDNHostnames")) {
            cfg->keep_fqdn = parse_bool(ptr);

        } else if (0 == strcmp(line, "DVMNetworks")) {
            if (NULL != cfg->dvm_networks) {
                free(cfg->dvm_networks);
            }
            cfg->dvm_networks = strdup(ptr);

        } else if (0 == strcmp(line, "DVMNetmask")) {
            if (NULL != cfg->dvm_netmask) {
                free(cfg->dvm_netmask);
            }
            cfg->dvm_netmask = strdup(ptr);

        } else if (0 == strcmp(line, "DVMRetryMaxDelay")) {
            cfg->retry_max_delay = strtoul(ptr, NULL, 10);

        } else if (0 == strcmp(line, "DVMTempDir")) {
            if (NULL != cfg->dvmtmpdir) {
                free(cfg->dvmtmpdir);
            }
            cfg->dvmtmpdir = strdup(ptr);

        } else if (0 == strcmp(line, "SessionTmpDir")) {
            if (NULL != cfg->sessiontmpdir) {
                free(cfg->sessiontmpdir);
            }
            cfg->sessiontmpdir = strdup(ptr);

        } else if (0 == strcmp(line, "ControllerLogPath")) {
            if (NULL != cfg->ctrllogpath) {
                free(cfg->ctrllogpath);
            }
            cfg->ctrllogpath = strdup(ptr);

        } else if (0 == strcmp(line, "PRTEDLogPath")) {
            if (NULL != cfg->prtedlogpath) {
                free(cfg->prtedlogpath);
            }
            cfg->prtedlogpath = strdup(ptr);

        } else if (0 == strcmp(line, "ControllerLogJobState")) {
            cfg->ctrl_log_jobstate = parse_bool(ptr);

        } else if (0 == strcmp(line, "ControllerLogProcState")) {
            cfg->ctrl_log_procstate = parse_bool(ptr);

        } else if (0 == strcmp(line, "PRTEDLogJobState")) {
            cfg->prted_log_jobstate = parse_bool(ptr);

        } else if (0 == strcmp(line, "PRTEDLogProcState")) {
            cfg->prted_log_procstate = parse_bool(ptr);
        }
        /* unknown keys are silently ignored for forward compatibility */
        free(line);
    }
    fclose(fp);

    /* we require the node list */
    if (NULL == dvmnodes) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-missing-entry", true,
                       prte_process_info.nodename, path, "DVMNodes");
        goto cleanup;
    }
    /* we must be able to parse that list */
    ret = regex_extract_nodes(dvmnodes, &cfg->nodes);
    if (PMIX_SUCCESS != ret) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-bad-nodelist", true,
                       prte_process_info.nodename, path, dvmnodes, PMIx_Error_string(ret));
        goto cleanup;
    }
    /* we must have the controller host so we can find it */
    if (NULL == cfg->ctrlhost) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-missing-entry", true,
                       prte_process_info.nodename, path, "DVMControllerHost");
        goto cleanup;
    }
    /* the IP version must be sane */
    if (4 != cfg->ip_version && 6 != cfg->ip_version) {
        pmix_show_help("help-prte-runtime.txt", "bootstrap-bad-entry", true,
                       prte_process_info.nodename, path, "DVMIPVersion");
        goto cleanup;
    }

    free(path);
    if (NULL != dvmnodes) {
        free(dvmnodes);
    }
    return PRTE_SUCCESS;

cleanup:
    free(path);
    if (NULL != dvmnodes) {
        free(dvmnodes);
    }
    prte_bootstrap_config_free(cfg);
    return rc;
}

void prte_bootstrap_config_free(prte_bootstrap_config_t *cfg)
{
    if (NULL == cfg) {
        return;
    }
    if (NULL != cfg->cluster) {
        free(cfg->cluster);
    }
    if (NULL != cfg->ctrlhost) {
        free(cfg->ctrlhost);
    }
    if (NULL != cfg->nodes) {
        PMIx_Argv_free(cfg->nodes);
    }
    if (NULL != cfg->dvm_networks) {
        free(cfg->dvm_networks);
    }
    if (NULL != cfg->dvm_netmask) {
        free(cfg->dvm_netmask);
    }
    if (NULL != cfg->dvmtmpdir) {
        free(cfg->dvmtmpdir);
    }
    if (NULL != cfg->sessiontmpdir) {
        free(cfg->sessiontmpdir);
    }
    if (NULL != cfg->ctrllogpath) {
        free(cfg->ctrllogpath);
    }
    if (NULL != cfg->prtedlogpath) {
        free(cfg->prtedlogpath);
    }
    memset(cfg, 0, sizeof(*cfg));
}

pmix_rank_t prte_bootstrap_num_daemons(prte_bootstrap_config_t *cfg)
{
    pmix_rank_t n = 0;
    bool ctrl_in_list = false;
    int i;

    if (NULL == cfg->nodes) {
        return 0;
    }
    for (i = 0; NULL != cfg->nodes[i]; i++) {
        n++;
        if (NULL != cfg->ctrlhost && host_match(cfg->nodes[i], cfg->ctrlhost, cfg->keep_fqdn)) {
            ctrl_in_list = true;
        }
    }
    /* the controller is rank 0 whether or not it appears in the list; if it
     * is not in the list it is an extra (management) node */
    return ctrl_in_list ? n : (n + 1);
}

int prte_bootstrap_rank_of(prte_bootstrap_config_t *cfg, const char *name, pmix_rank_t *rank)
{
    pmix_rank_t r = 1;
    int i;

    if (NULL != cfg->ctrlhost && host_match(name, cfg->ctrlhost, cfg->keep_fqdn)) {
        *rank = 0;
        return PRTE_SUCCESS;
    }
    if (NULL == cfg->nodes) {
        return PRTE_ERR_NOT_FOUND;
    }
    for (i = 0; NULL != cfg->nodes[i]; i++) {
        if (NULL != cfg->ctrlhost && host_match(cfg->nodes[i], cfg->ctrlhost, cfg->keep_fqdn)) {
            /* the controller is rank 0, not a numbered position */
            continue;
        }
        if (host_match(cfg->nodes[i], name, cfg->keep_fqdn)) {
            *rank = r;
            return PRTE_SUCCESS;
        }
        r++;
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_bootstrap_host_of_rank(prte_bootstrap_config_t *cfg, pmix_rank_t rank,
                                const char **host)
{
    pmix_rank_t r = 1;
    int i;

    if (0 == rank) {
        if (NULL == cfg->ctrlhost) {
            return PRTE_ERR_NOT_FOUND;
        }
        *host = cfg->ctrlhost;
        return PRTE_SUCCESS;
    }
    if (NULL == cfg->nodes) {
        return PRTE_ERR_NOT_FOUND;
    }
    for (i = 0; NULL != cfg->nodes[i]; i++) {
        if (NULL != cfg->ctrlhost && host_match(cfg->nodes[i], cfg->ctrlhost, cfg->keep_fqdn)) {
            /* the controller is rank 0, not a numbered position */
            continue;
        }
        if (r == rank) {
            *host = cfg->nodes[i];
            return PRTE_SUCCESS;
        }
        r++;
    }
    return PRTE_ERR_NOT_FOUND;
}

int prte_bootstrap_my_identity(prte_bootstrap_config_t *cfg, bool *is_controller,
                               pmix_rank_t *rank)
{
    pmix_rank_t r = 1;
    int i;

    if (NULL != cfg->ctrlhost && is_local(cfg, cfg->ctrlhost)) {
        *is_controller = true;
        *rank = 0;
        return PRTE_SUCCESS;
    }
    if (NULL == cfg->nodes) {
        return PRTE_ERR_NOT_FOUND;
    }
    for (i = 0; NULL != cfg->nodes[i]; i++) {
        if (NULL != cfg->ctrlhost && host_match(cfg->nodes[i], cfg->ctrlhost, cfg->keep_fqdn)) {
            continue;
        }
        if (is_local(cfg, cfg->nodes[i])) {
            *is_controller = false;
            *rank = r;
            return PRTE_SUCCESS;
        }
        r++;
    }
    return PRTE_ERR_NOT_FOUND;
}

static pmix_status_t regex_extract_nodes(char *regexp, char ***names)
{
    int i, j, k, len;
    pmix_status_t ret;
    char *base;
    char *orig, *suffix;
    bool found_range = false;
    bool more_to_come = false;
    int num_digits;

    /* set the default */
    *names = NULL;

    if (NULL == regexp) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* see what regex we were given. Supported options:
     *
     * file:<path> - file of names, one per line
     * <regex> - PMIx native regex, which is a comma-delimited
     *                list of ranges or names
     */
    if (0 == strncasecmp(regexp, "file:", 5)) {
        /* skip over the "file:" portion */
        ret = read_file(&regexp[5], names);
        return ret;
    }

    orig = base = strdup(regexp);
    if (NULL == base) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    do {
        /* Find the base */
        len = strlen(base);
        for (i = 0; i <= len; ++i) {
            if (base[i] == '[') {
                /* we found a range. this gets dealt with below */
                base[i] = '\0';
                found_range = true;
                break;
            }
            if (base[i] == ',') {
                /* we found a singleton value, and there are more to come */
                base[i] = '\0';
                found_range = false;
                more_to_come = true;
                break;
            }
            if (base[i] == '\0') {
                /* we found a singleton value */
                found_range = false;
                more_to_come = false;
                break;
            }
        }
        if (i == 0 && !found_range) {
            /* we found a special character at the beginning of the string */
            free(orig);
            return PMIX_ERR_BAD_PARAM;
        }

        if (found_range) {
            /* If we found a range, get the number of digits in the numbers */
            i++; /* step over the [ */
            for (j = i; j < len; j++) {
                if (base[j] == ':') {
                    base[j] = '\0';
                    break;
                }
            }
            if (j >= len) {
                /* we didn't find the number of digits */
                free(orig);
                return PMIX_ERR_BAD_PARAM;
            }
            num_digits = strtol(&base[i], NULL, 10);
            i = j + 1; /* step over the : */
            /* now find the end of the range */
            for (j = i; j < len; ++j) {
                if (base[j] == ']') {
                    base[j] = '\0';
                    break;
                }
            }
            if (j >= len) {
                /* we didn't find the end of the range */
                free(orig);
                return PMIX_ERR_BAD_PARAM;
            }
            /* check for a suffix */
            if (j + 1 < len && base[j + 1] != ',') {
                /* find the next comma, if present */
                for (k = j + 1; k < len && base[k] != ','; k++)
                    ;
                if (k < len) {
                    base[k] = '\0';
                }
                suffix = strdup(&base[j + 1]);
                if (k < len) {
                    base[k] = ',';
                }
                j = k - 1;
            } else {
                suffix = NULL;
            }

            ret = regex_parse_value_ranges(base, base + i, num_digits, suffix, names);
            if (NULL != suffix) {
                free(suffix);
            }
            if (PMIX_SUCCESS != ret) {
                free(orig);
                return ret;
            }
            if (j + 1 < len && base[j + 1] == ',') {
                more_to_come = true;
                base = &base[j + 2];
            } else {
                more_to_come = false;
            }
        } else {
            /* If we didn't find a range, just add the value */
            PMIx_Argv_append_nosize(names, base);
            /* step over the comma */
            i++;
            /* set base equal to the (possible) next base to look at */
            base = &base[i];
        }
    } while (more_to_come);

    free(orig);

    /* All done */
    return PRTE_SUCCESS;
}

/*
 * Parse one or more ranges in a set
 */
static pmix_status_t regex_parse_value_ranges(char *base, char *ranges, int num_digits,
                                              char *suffix, char ***names)
{
    int i, len;
    pmix_status_t ret;
    char *start, *orig;

    /* Look for commas, the separator between ranges */
    len = strlen(ranges);
    for (orig = start = ranges, i = 0; i < len; ++i) {
        if (',' == ranges[i]) {
            ranges[i] = '\0';
            ret = regex_parse_value_range(base, start, num_digits, suffix, names);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                return ret;
            }
            start = ranges + i + 1;
        }
    }

    /* Pick up the last range, if it exists */
    if (start < orig + len) {
        ret = regex_parse_value_range(base, start, num_digits, suffix, names);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            return ret;
        }
    }

    /* All done */
    return PMIX_SUCCESS;
}

/*
 * Parse a single range in a set and add the full names of the values
 * found to the names argv
 */
static pmix_status_t regex_parse_value_range(char *base, char *range, int num_digits,
                                             char *suffix, char ***names)
{
    char *str, tmp[132];
    size_t i, k, start, end;
    size_t base_len, len;
    bool found;

    if (NULL == base || NULL == range) {
        return PMIX_ERROR;
    }

    len = strlen(range);
    base_len = strlen(base);
    /* Silence compiler warnings; start and end are always assigned
     properly, below */
    start = end = 0;

    /* Look for the beginning of the first number */
    for (found = false, i = 0; i < len; ++i) {
        if (isdigit((int) range[i])) {
            if (!found) {
                start = strtol(range + i, NULL, 10);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        return PMIX_ERR_NOT_FOUND;
    }

    /* Look for the end of the first number */
    for (found = false; i < len; ++i) {
        if (!isdigit(range[i])) {
            break;
        }
    }

    /* Was there no range, just a single number? */
    if (i >= len) {
        end = start;
        found = true;
    } else {
        /* Nope, there was a range.  Look for the beginning of the second
         * number
         */
        for (; i < len; ++i) {
            if (isdigit(range[i])) {
                end = strtol(range + i, NULL, 10);
                found = true;
                break;
            }
        }
    }
    if (!found) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        return PMIX_ERR_NOT_FOUND;
    }

    /* Make strings for all values in the range */
    len = base_len + num_digits + 32;
    if (NULL != suffix) {
        len += strlen(suffix);
    }
    str = (char *) malloc(len);
    if (NULL == str) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    for (i = start; i <= end; ++i) {
        memset(str, 0, len);
        strcpy(str, base);
        /* we need to zero-pad the digits */
        for (k = 0; k < (size_t) num_digits; k++) {
            str[k + base_len] = '0';
        }
        memset(tmp, 0, 132);
        pmix_snprintf(tmp, 132, "%lu", (unsigned long) i);
        for (k = 0; k < strlen(tmp); k++) {
            str[base_len + num_digits - k - 1] = tmp[strlen(tmp) - k - 1];
        }
        /* if there is a suffix, add it */
        if (NULL != suffix) {
            strcat(str, suffix);
        }
        PMIx_Argv_append_nosize(names, str);
    }
    free(str);

    /* All done */
    return PMIX_SUCCESS;
}

static pmix_status_t read_file(char *regexp, char ***names)
{
    char *line;
    FILE *fp;

    fp = fopen(regexp, "r");
    if (NULL == fp) {
        return PMIX_ERR_BAD_PARAM;
    }
    while (NULL != (line = pmix_getline(fp))) {
        /* ignore if line is empty or comment */
        if (0 == strlen(line) || '#' == line[0]) {
            free(line);
            continue;
        }
        PMIx_Argv_append_nosize(names, line);
        free(line);
    }
    fclose(fp);
    return PMIX_SUCCESS;
}
