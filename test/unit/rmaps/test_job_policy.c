/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Tests the job-level --map-by / --rank-by parsers and the policy printer.
 *
 * The per-app parser is covered by test_policy_parse.c.  This is its
 * job-level twin - the path an ordinary single-app command line takes,
 * which is where a policy plus qualifiers ("package:PE=2") and the ppr
 * pattern ("ppr:2:core") are actually written down, and where the
 * rankfile/pe-list specs are accepted at all.
 *
 * The printer is here too because every mapper's error message and the map
 * display run the resolved policy back through it; a policy word that comes
 * back short is a message that misreports what the job asked for.
 */

#include "prte_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/runtime/prte_globals.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/util/attr.h"

int test_job_policy(void);

#define CHECK(label, cond)                                              \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL [%s]: %s\n", label, #cond);          \
            failures++;                                                 \
        }                                                               \
    } while (0)

static prte_job_t *newjob(void)
{
    prte_job_t *jdata = PMIX_NEW(prte_job_t);
    jdata->map = PMIX_NEW(prte_job_map_t);
    return jdata;
}

static uint16_t get_u16(pmix_list_t *attrs, prte_attribute_key_t key)
{
    uint16_t val = 0;
    uint16_t *ptr = &val;
    if (!prte_get_attribute(attrs, key, (void **) &ptr, PMIX_UINT16)) {
        return 0;
    }
    return val;
}

static char *get_str(pmix_list_t *attrs, prte_attribute_key_t key)
{
    char *str = NULL;
    prte_get_attribute(attrs, key, (void **) &str, PMIX_STRING);
    return str;
}

int test_job_policy(void)
{
    int failures = 0;
    int rc;
    prte_job_t *jdata;
    char *sval;
    char *printed;

    /* === a bare policy === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "package");
    CHECK("job package: rc", PRTE_SUCCESS == rc);
    CHECK("job package: policy",
          PRTE_MAPPING_BYPACKAGE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    CHECK("job package: GIVEN",
          PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
    PMIX_RELEASE(jdata);

    /* === a policy with qualifiers.  The qualifier list is split off,
     * parsed, and must not be mistaken for the policy word === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "package:PE=2:SPAN");
    CHECK("job package:PE=2:SPAN: rc", PRTE_SUCCESS == rc);
    CHECK("job package:PE=2:SPAN: policy",
          PRTE_MAPPING_BYPACKAGE == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    CHECK("job package:PE=2:SPAN: span",
          PRTE_MAPPING_SPAN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
    CHECK("job package:PE=2:SPAN: pes", 2 == get_u16(&jdata->attributes,
                                                     PRTE_JOB_PES_PER_PROC));
    PMIX_RELEASE(jdata);

    /* === a qualifier may be abbreviated, and its value must survive that.
     * The matcher accepts any unambiguous prefix, so the value cannot be
     * read at an offset fixed to the full spelling: "P=2" used to yield
     * pes-per-proc 0 and "F=path" used to open the path five characters
     * in === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "slot:P=2");
    CHECK("job slot:P=2: rc", PRTE_SUCCESS == rc);
    CHECK("job slot:P=2: pes", 2 == get_u16(&jdata->attributes, PRTE_JOB_PES_PER_PROC));
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "seq:F=/tmp/seqfile");
    CHECK("job seq:F=path: rc", PRTE_SUCCESS == rc);
    sval = get_str(&jdata->attributes, PRTE_JOB_FILE);
    CHECK("job seq:F=path: whole path", NULL != sval && 0 == strcmp(sval, "/tmp/seqfile"));
    free(sval);
    PMIX_RELEASE(jdata);

    /* === a qualifier that needs a value and has none is refused, not
     * quietly read as zero === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "slot:PE=");
    CHECK("job slot:PE= (empty): refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "slot:PE=two");
    CHECK("job slot:PE=two: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "seq:FILE=");
    CHECK("job seq:FILE= (empty): refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === NOLOCAL and ORDERED === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "core:NOLOCAL:ORDERED");
    CHECK("job core:NOLOCAL:ORDERED: rc", PRTE_SUCCESS == rc);
    CHECK("job core:NOLOCAL:ORDERED: nolocal",
          PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
    CHECK("job core:NOLOCAL:ORDERED: ordered",
          PRTE_MAPPING_ORDERED & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
    PMIX_RELEASE(jdata);

    /* === a spec that is qualifiers only names no policy === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, ":OVERSUBSCRIBE");
    CHECK("job :OVERSUBSCRIBE: rc", PRTE_SUCCESS == rc);
    CHECK("job :OVERSUBSCRIBE: no policy",
          0 == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    CHECK("job :OVERSUBSCRIBE: given",
          PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping));
    CHECK("job :OVERSUBSCRIBE: allowed",
          !(PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)));
    PMIX_RELEASE(jdata);

    /* === ppr: the pattern is stored whole, and modifiers after it still
     * get parsed === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "ppr:2:core");
    CHECK("job ppr:2:core: rc", PRTE_SUCCESS == rc);
    CHECK("job ppr:2:core: policy",
          PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    sval = get_str(&jdata->attributes, PRTE_JOB_PPR);
    CHECK("job ppr:2:core: pattern", NULL != sval && 0 == strcmp(sval, "2:core"));
    free(sval);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "ppr:1:package:PE=2");
    CHECK("job ppr+PE: rc", PRTE_SUCCESS == rc);
    CHECK("job ppr+PE: policy",
          PRTE_MAPPING_PPR == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    CHECK("job ppr+PE: pes", 2 == get_u16(&jdata->attributes, PRTE_JOB_PES_PER_PROC));
    sval = get_str(&jdata->attributes, PRTE_JOB_PPR);
    CHECK("job ppr+PE: pattern", NULL != sval && 0 == strcmp(sval, "1:package"));
    free(sval);
    PMIX_RELEASE(jdata);

    /* === a ppr with no object to go with the count === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "ppr:2");
    CHECK("job ppr:2: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === pe-list: the ranges are validated and kept as the job cpuset === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "pe-list=0-3,6");
    CHECK("job pe-list: rc", PRTE_SUCCESS == rc);
    CHECK("job pe-list: policy",
          PRTE_MAPPING_PELIST == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    sval = get_str(&jdata->attributes, PRTE_JOB_CPUSET);
    CHECK("job pe-list: cpuset", NULL != sval && 0 == strcmp(sval, "0-3,6"));
    free(sval);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "pe-list=0-3-5");
    CHECK("job pe-list bad range: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "pe-list=zero");
    CHECK("job pe-list non-numeric: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === rankfile without a file has nothing to map from === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "rankfile");
    CHECK("job rankfile, no file: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "rankfile:FILE=/tmp/rf");
    CHECK("job rankfile:FILE: rc", PRTE_SUCCESS == rc);
    CHECK("job rankfile:FILE: policy",
          PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(jdata->map->mapping));
    sval = get_str(&jdata->attributes, PRTE_JOB_FILE);
    CHECK("job rankfile:FILE: path", NULL != sval && 0 == strcmp(sval, "/tmp/rf"));
    free(sval);
    PMIX_RELEASE(jdata);

    /* === garbage is refused, not guessed at === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "banana");
    CHECK("job banana: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, "core:banana");
    CHECK("job core:banana: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === a NULL spec is a no-op === */
    jdata = newjob();
    rc = prte_rmaps_base_set_mapping_policy(jdata, NULL);
    CHECK("job NULL spec: rc", PRTE_SUCCESS == rc);
    CHECK("job NULL spec: untouched", 0 == jdata->map->mapping);
    PMIX_RELEASE(jdata);

    /* === ranking === */
    jdata = newjob();
    rc = prte_rmaps_base_set_ranking_policy(jdata, "span");
    CHECK("job rankby span: rc", PRTE_SUCCESS == rc);
    CHECK("job rankby span: policy",
          PRTE_RANK_BY_SPAN == PRTE_GET_RANKING_POLICY(jdata->map->ranking));
    CHECK("job rankby span: GIVEN",
          PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(jdata->map->ranking));
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_rmaps_base_set_ranking_policy(jdata, "banana");
    CHECK("job rankby banana: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === a NULL ranking spec derives the default from the mapping === */
    jdata = newjob();
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYNODE);
    rc = prte_rmaps_base_set_ranking_policy(jdata, NULL);
    CHECK("job rankby default from bynode: rc", PRTE_SUCCESS == rc);
    CHECK("job rankby default from bynode: node",
          PRTE_RANK_BY_NODE == PRTE_GET_RANKING_POLICY(jdata->map->ranking));
    PMIX_RELEASE(jdata);

    jdata = newjob();
    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYCORE);
    rc = prte_rmaps_base_set_ranking_policy(jdata, NULL);
    CHECK("job rankby default from bycore: fill",
          PRTE_RANK_BY_FILL == PRTE_GET_RANKING_POLICY(jdata->map->ranking));
    PMIX_RELEASE(jdata);

    /* === the job-level --bind-to parser reads its LIMIT= value the same
     * way, and lives in src/hwloc rather than here.  It is tested from this
     * file because the abbreviation rule is the same rule: the qualifier
     * name may be shortened, so the value cannot live at a fixed offset,
     * and "L=1" used to read two bytes past the end of its own string === */
    jdata = newjob();
    rc = prte_hwloc_base_set_binding_policy(jdata, "core:LIMIT=2");
    CHECK("bindto core:LIMIT=2: rc", PRTE_SUCCESS == rc);
    CHECK("bindto core:LIMIT=2: limit", 2 == get_u16(&jdata->attributes,
                                                     PRTE_JOB_BINDING_LIMIT));
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_hwloc_base_set_binding_policy(jdata, "core:L=2");
    CHECK("bindto core:L=2: rc", PRTE_SUCCESS == rc);
    CHECK("bindto core:L=2: limit", 2 == get_u16(&jdata->attributes,
                                                 PRTE_JOB_BINDING_LIMIT));
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_hwloc_base_set_binding_policy(jdata, "core:LIMIT=");
    CHECK("bindto core:LIMIT= (empty): refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    jdata = newjob();
    rc = prte_hwloc_base_set_binding_policy(jdata, "core:LIMIT=two");
    CHECK("bindto core:LIMIT=two: refused", PRTE_SUCCESS != rc);
    PMIX_RELEASE(jdata);

    /* === the printer.  Every qualifier at once is the longest string it
     * can be asked to produce, and it has to come back whole === */
    {
        prte_mapping_policy_t pol = 0;
        PRTE_SET_MAPPING_POLICY(pol, PRTE_MAPPING_BYHWTHREAD);
        PRTE_SET_MAPPING_DIRECTIVE(pol, PRTE_MAPPING_NO_USE_LOCAL);
        PRTE_SET_MAPPING_DIRECTIVE(pol, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        PRTE_SET_MAPPING_DIRECTIVE(pol, PRTE_MAPPING_SPAN);
        PRTE_SET_MAPPING_DIRECTIVE(pol, PRTE_MAPPING_ORDERED);
        printed = prte_rmaps_base_print_mapping(pol);
        CHECK("print: policy word", NULL != printed
              && 0 == strncmp(printed, "BYHWTHREAD", strlen("BYHWTHREAD")));
        CHECK("print: not truncated", NULL != printed
              && NULL != strstr(printed, "NO_USE_LOCAL")
              && NULL != strstr(printed, "NOOVERSUBSCRIBE")
              && NULL != strstr(printed, "SPAN")
              && NULL != strstr(printed, "ORDERED"));
    }

    printed = prte_rmaps_base_print_mapping(PRTE_MAPPING_BYSLOT);
    CHECK("print: bare byslot", NULL != printed && 0 == strcmp(printed, "BYSLOT"));

    printed = prte_rmaps_base_print_ranking(PRTE_RANK_BY_FILL);
    CHECK("print: rank fill", NULL != printed && 0 == strcmp(printed, "FILL"));

    if (0 == failures) {
        fprintf(stdout, "  PASS test_job_policy\n");
    }
    return failures;
}
