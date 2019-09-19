/* -*- C -*-
 *
 * $HEADER$
 *
 * A test to trap user signals
 */
#include "prrte_config.h"

#include <stdio.h>
#include <signal.h>

#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/runtime.h"

/* yeah, we know it isn't safe to call fprintf inside signal handlers,
 * but this is good enough for this test
 */
void sigusr_handler(int signum)
{
    switch (signum) {
        case SIGUSR1:
            fprintf(stderr, "%s Trapped SIGUSR1\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            break;

        case SIGUSR2:
            fprintf(stderr, "%s Trapped SIGUSR2\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            return;

        case SIGCONT:
            fprintf(stderr, "%s Trapped SIGCONT\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            return;

        default:
            fprintf(stderr, "%s Undefined signal %d trapped\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), signum);
            return;
    }
}

void exit_handler(int signum)
{
    int rc;

    switch (signum) {
        case SIGINT:
            fprintf(stderr, "%s Trapped SIGINT\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            break;

        case SIGHUP:
            fprintf(stderr, "%s Trapped SIGHUP\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            break;

        case SIGTERM:
            fprintf(stderr, "%s Trapped SIGTERM\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            break;

        default:
            fprintf(stderr, "%s Undefined signal %d trapped\n", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), signum);
            break;
    }
    return;

    exit(1);
}


int main(int argc, char* argv[])
{

    int rc;
    int i;
    double pi;

    prrte_init(&argc, &argv, PRRTE_PROC_TOOL);

    if (signal(SIGUSR1, sigusr_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGUSR1\n");
        exit(1);
    }

    if (signal(SIGUSR2, sigusr_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGUSR2\n");
        exit(1);
    }

    if (signal(SIGCONT, sigusr_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGUSR2\n");
        exit(1);
    }

    if (signal(SIGINT, exit_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGINT\n");
        exit(1);
    }

    if (signal(SIGHUP, exit_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGHUP\n");
        exit(1);
    }

    if (signal(SIGTERM, exit_handler) == SIG_IGN) {
        fprintf(stderr, "Could not setup signal trap for SIGTERM\n");
        exit(1);
    }

    i = 0;
    while (1) {
        i++;
        pi = i / 3.14159256;
        if (i > 100) i = 0;
    }

    return 0;
}
