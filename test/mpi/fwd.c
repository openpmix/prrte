#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>
#include <stdbool.h>

#define SPAWN 1

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);

    MPI_Comm parent = MPI_COMM_NULL;
    MPI_Comm_get_parent(&parent);

    bool am_parent = parent == MPI_COMM_NULL;
    bool second_child = false;

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (SPAWN)
    {
        if (am_parent)
        {
            MPI_Info the_info;
            MPI_Info_create(&the_info);
            MPI_Info_set(the_info, "PMIX_MAPBY", "PPR:1:NODE");

            char *args[] = {"GO", NULL}; // Just so we can know when to quit spawning
            MPI_Comm_spawn(argv[0], args, 2, the_info, 0, MPI_COMM_WORLD, &parent, MPI_ERRCODES_IGNORE);

            MPI_Info_free(&the_info);
        }
        else
        {
            if (argc > 1)
            {
                MPI_Info the_info;
                MPI_Info_create(&the_info);
                MPI_Info_set(the_info, "PMIX_MAPBY", "PPR:1:NODE");
                MPI_Comm_spawn(argv[0], MPI_ARGV_NULL, 2, the_info, 0, MPI_COMM_WORLD, &parent, MPI_ERRCODES_IGNORE);
                MPI_Info_free(&the_info);
            }
            else
            {
                second_child = true;
            }
        }
    }

    const char *vars[] = {"DUMMY_VAR"};
    const int nvars = sizeof(vars) / sizeof(vars[0]);

    char processor[MPI_MAX_PROCESSOR_NAME];
    int len_ignore;

    MPI_Get_processor_name(processor, &len_ignore);

    for (int i = 0; i < nvars; i++)
    {
        char *value = getenv(vars[i]);
        if (value)
        {
            printf("Hi from rank %d on %s. Child? %s%s. Environment variable %s found with value: %s\n",
                   rank, processor, am_parent ? "F" : "T", second_child ? " (second)" : "", vars[i], value);
        }
        else
        {
            printf("Hi from rank %d on %s. Child? %s%s. Environment variable %s NOT found.\n",
                   rank, processor, am_parent ? "F" : "T", second_child ? " (second)" : "", vars[i]);
        }
    }

    MPI_Finalize();

    return 0;
}

