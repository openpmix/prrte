#include <stdio.h>
#include <pmix.h>
#include <sched.h>

static pmix_proc_t allproc = {};
static pmix_proc_t myproc = {};

#define ERR(msg, ...)							\
    do {								\
	time_t tm = time(NULL);						\
	char *stm = ctime(&tm);						\
	stm[strlen(stm)-1] = 0;						\
	fprintf(stderr, "%s ERROR: %s:%d  " msg "\n", stm, __FILE__, __LINE__, ## __VA_ARGS__); \
	exit(1);							\
    } while(0);


int pmi_set_string(const char *key, void *data, size_t size)
{
    int rc;
    pmix_value_t value;

    PMIX_VALUE_CONSTRUCT(&value);
    value.type = PMIX_BYTE_OBJECT;
    value.data.bo.bytes = data;
    value.data.bo.size  = size;
    if (PMIX_SUCCESS != (rc = PMIx_Put(PMIX_REMOTE, key, &value))) {
        ERR("Client ns %s rank %d: PMIx_Put failed: %s\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    }

    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        ERR("Client ns %s rank %d: PMIx_Commit failed: %s\n", myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    }

    /* protect the data */
    value.data.bo.bytes = NULL;
    value.data.bo.size  = 0;
    PMIX_VALUE_DESTRUCT(&value);
    printf("%s:%d PMIx_Put on %s\n", myproc.nspace, myproc.rank, key);


    return 0;
}

int pmi_get_string(uint32_t peer_rank, const char *key, bool refresh, void **data_out, size_t *data_size_out)
{
    int rc;
    pmix_proc_t proc;
    pmix_value_t *pvalue;
    pmix_info_t info;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer_rank);
    PMIX_INFO_LOAD(&info, PMIX_GET_REFRESH_CACHE, &refresh, PMIX_BOOL);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, key, &info, 1, &pvalue))) {
        ERR("Client ns %s rank %d: PMIx_Get on rank %u %s: %s\n", myproc.nspace, myproc.rank, peer_rank, key, PMIx_Error_string(rc));
    }
    if(pvalue->type != PMIX_BYTE_OBJECT){
        ERR("Client ns %s rank %d: PMIx_Get %s: got wrong data type\n", myproc.nspace, myproc.rank, key);
    }
    *data_out = pvalue->data.bo.bytes;
    *data_size_out = pvalue->data.bo.size;

    /* protect the data */
    pvalue->data.bo.bytes = NULL;
    pvalue->data.bo.size = 0;
    PMIX_VALUE_RELEASE(pvalue);
    PMIX_PROC_DESTRUCT(&proc);

    printf("%s:%d PMIx_get %s returned %zi bytes\n", myproc.nspace, myproc.rank, key, data_size_out[0]);

    return 0;
}

int pmix_exchange(bool flag)
{
    int rc;
    pmix_info_t info;

    fprintf(stderr, "Execute fence\n");
    PMIX_INFO_CONSTRUCT(&info);
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&allproc, 1, &info, 1);
    if (PMIX_SUCCESS != rc){
        fprintf(stderr, "Client ns %s rank %d: PMIx_Fence_nb failed: %d\n", myproc.nspace, myproc.rank, rc);
        exit(1);
    }
    PMIX_INFO_DESTRUCT(&info);


    return 0;
}

int main(int argc, char *argv[])
{
    char data[256] = {};
    char *data_out;
    size_t size_out;
    int rc;
    pmix_value_t *pvalue;

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
	ERR("PMIx_Init failed");
        exit(1);
    }
    if(myproc.rank == 0) printf("PMIx initialized\n");

    /* job-related info is found in our nspace, assigned to the
     * wildcard rank as it doesn't relate to a specific rank. Setup
     * a name to retrieve such values */
    PMIX_LOAD_PROCID(&allproc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* get the number of procs in our job */
    if (PMIX_SUCCESS != (rc = PMIx_Get(&allproc, PMIX_JOB_SIZE, NULL, 0, &pvalue))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Get job size failed: %d\n", myproc.nspace, myproc.rank, rc);
        exit(1);
    }
    uint32_t nprocs = pvalue->data.uint32;
    PMIX_VALUE_RELEASE(pvalue);

    /* the below two lines break the subsequent PMIx_Get query on a key set later */
    sprintf(data, "FIRST TIME rank %d", myproc.rank);
    pmi_set_string("test-key-1", data, 256);
    pmix_exchange(true);

    sprintf(data, "SECOND TIME rank %d", myproc.rank);
    pmi_set_string("test-key-2", data, 256);

    /* An explicit Fence has to be called again to read the `test-key-2` */
    // pmix_exchange(false);

    pmi_get_string((myproc.rank+1)%2, "test-key-2", true, (void**)&data_out, &size_out);
    printf("%d: obtained data \"%s\"\n", myproc.rank, data_out);

    if (PMIX_SUCCESS != (rc = PMIx_Finalize(NULL, 0))) {
        ERR("Client ns %s rank %d:PMIx_Finalize failed: %d\n", myproc.nspace, myproc.rank, rc);
    }
    if(myproc.rank == 0) printf("PMIx finalized\n");

    exit(0);
}
