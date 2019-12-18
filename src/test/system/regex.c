/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include "prrte_config.h"

#include <stdio.h>
#include <unistd.h>

#include "src/util/argv.h"

#include "src/util/proc_info.h"
#include "src/util/regex.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/runtime.h"

int main(int argc, char **argv)
{
    int rc;
    char *regex, *save;
    char **nodes=NULL;
    int i;

    if (argc < 1 || NULL == argv[1]) {
        fprintf(stderr, "usage: regex <comma-separated list of nodes>\n");
        return 1;
    }

    prrte_init(&argc, &argv, PRRTE_PROC_NON_MPI);

    if (NULL != strchr(argv[1], '[')) {
        /* given a regex to analyze */
        fprintf(stderr, "ANALYZING REGEX: %s\n", argv[1]);
        if (PRRTE_SUCCESS != (rc = prrte_regex_extract_node_names(argv[1], &nodes))) {
            PRRTE_ERROR_LOG(rc);
        }
        for (i=0; NULL != nodes[i]; i++) {
            fprintf(stderr, "%s\n", nodes[i]);
        }
        opal_argv_free(nodes);
        prrte_finalize();
        return 0;
    }

    save = strdup(argv[1]);
    if (PRRTE_SUCCESS != (rc = prrte_regex_create(save, &regex))) {
        PRRTE_ERROR_LOG(rc);
    } else {
        fprintf(stderr, "REGEX: %s\n", regex);
        if (PRRTE_SUCCESS != (rc = prrte_regex_extract_node_names(regex, &nodes))) {
            PRRTE_ERROR_LOG(rc);
        }
        free(regex);
        regex = opal_argv_join(nodes, ',');
        opal_argv_free(nodes);
        if (0 == strcmp(regex, argv[1])) {
            fprintf(stderr, "EXACT MATCH\n");
        } else {
            fprintf(stderr, "ERROR: %s\n", regex);
        }
        free(regex);
    }
    free(save);
}
