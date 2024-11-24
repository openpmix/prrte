#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

int main(int argc, char* argv[])
{
    int rc;
    int size, my_rank;
    MPI_Group world_group;
    MPI_Comm foo;
    char *err;
    int len;
    int ranks[2] = {0, 1};
    MPI_Group new_group;
    MPI_Comm new_communicator = MPI_COMM_NULL;
    int value;

    MPI_Init(&argc, &argv);

    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (3 > size)
    {
        printf("Please run this application with at least 3 processes.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

    MPI_Comm_group(MPI_COMM_WORLD, &world_group);

    // create an initial communicator from comm_world
    rc = MPI_Comm_create_from_group(world_group, "mygroup", MPI_INFO_NULL, MPI_ERRORS_ARE_FATAL, &foo);
    MPI_Error_string(rc, err, &len);
    printf("Process %d creating comm from mygroup result: %s(%d)\n", my_rank, err, rc);

    // Keep only the processes 0 and 1 in the new group.
    MPI_Group_incl(world_group, 2, ranks, &new_group);

    // Create a new communicator from that group of processes.
    if (my_rank < 2){
        rc = MPI_Comm_create_from_group(new_group, "mygroup2", MPI_INFO_NULL, MPI_ERRORS_ARE_FATAL, &new_communicator);
        MPI_Error_string(rc, err, &len);
        printf("Process %d succeeded %s(%d)\n", my_rank, err, rc);
    }

    // Do a broadcast between all processes
    MPI_Bcast(&value, 1, MPI_INT, 0, MPI_COMM_WORLD);
    printf("Process %d took part to the global communicator broadcast.\n", my_rank);

    // Let's wait all processes before proceeding to the second phase.
    MPI_Barrier(MPI_COMM_WORLD);

    // Do a broadcast only between the processes of the new communicator.
    if(new_communicator == MPI_COMM_NULL)
    {
        printf("Process %d did not take part to the new communicator broadcast.\n", my_rank);
    } else {
        MPI_Bcast(&value, 1, MPI_INT, 0, new_communicator);
        printf("Process %d took part to the new communicator broadcast.\n", my_rank);
    }

    MPI_Finalize();

    return EXIT_SUCCESS;
}
