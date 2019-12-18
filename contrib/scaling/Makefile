PROGS = prrte_no_op mpi_no_op mpi_memprobe

all: $(PROGS)

CFLAGS = -O

prrte_no_op: prrte_no_op.c
	prrtecc -o prrte_no_op prrte_no_op.c

mpi_no_op: mpi_no_op.c
	mpicc -o mpi_no_op mpi_no_op.c

mpi_memprobe: mpi_memprobe.c
	mpicc -o mpi_memprobe mpi_memprobe.c -lopen-pal -lopen-rte

clean:
	rm -f $(PROGS) *~
