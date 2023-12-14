#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <mntent.h>
#include <string.h>
 #include <fcntl.h>

//#define MOUNT_FILE "/etc/mtab"
#define MOUNT_FILE "/proc/mounts"

static int mountpoint(char *filename, char **fstype)
{
    struct stat s;
    struct mntent mnt;
    FILE *fp;
    dev_t dev;
    char buf[1024];
    int fd;

    fd = open(filename, O_RDONLY);
    if (0 > fd) {
        fprintf(stderr, "CANNOT OPEN %s\n", filename);
        return EINVAL;
    }
    if (fstat(fd, &s) != 0) {
        return EINVAL;
    }
    close(fd);

    dev = s.st_dev;

    if ((fp = setmntent(MOUNT_FILE, "r")) == NULL) {
        return EINVAL;
    }

    while (getmntent_r(fp, &mnt, buf, sizeof(buf))) {
        fprintf(stderr, "MNT: %s %s\n", mnt.mnt_fsname, mnt.mnt_dir);
        fd = open(mnt.mnt_dir, O_RDONLY);
        if (0 > fd) {
            // probably lack permissions
            continue;
        }
        if (fstat(fd, &s) != 0) {
            close(fd);
            continue;
        }

        if (s.st_dev == dev) {
            *fstype = strdup(mnt.mnt_type);
            close(fd);
            return 0;
        }
        close(fd);
    }

    endmntent(fp);

    // Should never reach here.
    return EINVAL;
}

int main(int argc, char **argv)
{
    int n, rc;
    char *fstype;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s file1 file2 ...\n", argv[0]);
        return 1;
    }

    for (n=1; NULL != argv[n]; n++) {
        rc = mountpoint(argv[n], &fstype);
        fprintf(stdout, "Return: %d File: %s FStype: %s\n", rc, argv[n], fstype);
        free(fstype);
    }

    return 0;
}
