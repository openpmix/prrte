#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/time.h>
#endif
#include <errno.h>

#include "src/threads/threads.h"
#include "src/runtime/opal.h"
#include "src/event/event-internal.h"

static prrte_event_base_t *my_base=NULL;
static opal_thread_t progress_thread;
static bool progress_thread_stop=false;
static int progress_thread_pipe[2];
static opal_mutex_t lock;
static opal_condition_t cond;
static bool active=false;
typedef struct {
    opal_object_t super;
    opal_event_t write_event;
} foo_caddy_t;
PRRTE_CLASS_INSTANCE(foo_caddy_t,
                   opal_object_t,
                   NULL, NULL);
static bool fd_written=false;

static void* progress_engine(opal_object_t *obj);
static void send_handler(int sd, short flags, void *arg);

int main(int argc, char **argv)
{
    char byte='a';
    struct timespec tp={0, 100};
    int count=0;
    foo_caddy_t *foo;

   /* Initialize the event library */
    opal_init(&argc, &argv);

    /* setup for threads */
    opal_event_use_threads();

    /* create a new base */
    my_base = prrte_event_base_create();

   /* launch a progress thread on that base*/
    pipe(progress_thread_pipe);
    PRRTE_CONSTRUCT(&lock, opal_mutex_t);
    PRRTE_CONSTRUCT(&cond, opal_condition_t);
    PRRTE_CONSTRUCT(&progress_thread, opal_thread_t);
    progress_thread.t_run = progress_engine;
    if (PRRTE_SUCCESS != opal_thread_start(&progress_thread)) {
        fprintf(stderr, "Unable to start progress thread\n");
        prrte_event_base_finalize(my_base);
        exit(1);
    }

    /* wait a little while - reflects reality in an async system */
    while (count < 100) {
        nanosleep(&tp, NULL);
        count++;
    }
    count=0;

    /* make a dummy event */
    fprintf(stderr, "activating the write_event");
    foo = PRRTE_NEW(foo_caddy_t);
    opal_event_set(my_base,
                   &foo->write_event,
                   -1,
                   0,
                   send_handler,
                   foo);
    /* activate it. */
    opal_event_active(&foo->write_event, EV_WRITE, 1);

    /* wait for it to trigger */
    while (!fd_written && count < 1000) {
        if (0 == (count % 100)) {
            fprintf(stderr, "Waiting...\n");
        }
        nanosleep(&tp, NULL);
        count++;
    }

    /* stop the thread */
    PRRTE_ACQUIRE_THREAD(&lock, &cond, &active);
    progress_thread_stop = true;
    PRRTE_RELEASE_THREAD(&lock, &cond, &active);
    opal_fd_write(progress_thread_pipe[1], 1, &byte);
    opal_thread_join(&progress_thread, NULL);

    /* release the base */
    fprintf(stderr, "Cleaning up\n");
    opal_finalize();
    fprintf(stderr, "Cleanup completed\n");
    return 0;
}

static struct event stop_event;
static void stop_handler(int sd, short flags, void* cbdata)
{
    char byte;

    opal_fd_read(progress_thread_pipe[0], 1, &byte);
    fprintf(stderr, "Stop handler called\n");
    /* reset the event */
    opal_event_add(&stop_event, 0);
    return;
}

static void* progress_engine(opal_object_t *obj)
{
    /* define an event that will be used to kick us out of a blocking
     * situation when we want to exit
     */
    /* define an event that will be used to kick us out of a blocking
     * situation when we want to exit
     */
    opal_event_set(my_base, &stop_event,
                   progress_thread_pipe[0], PRRTE_EV_READ, stop_handler, NULL);
    opal_event_add(&stop_event, 0);

    while (1) {
        PRRTE_ACQUIRE_THREAD(&lock, &cond, &active);
        if (progress_thread_stop) {
            fprintf(stderr, "Thread stopping\n");
            PRRTE_RELEASE_THREAD(&lock, &cond, &active);
            opal_event_del(&stop_event);
            return PRRTE_THREAD_CANCELLED;
        }
        PRRTE_RELEASE_THREAD(&lock, &cond, &active);
        fprintf(stderr, "Looping...\n");
        opal_event_loop(my_base, PRRTE_EVLOOP_ONCE);
    }
}

static void send_handler(int sd, short flags, void *arg)
{
    foo_caddy_t *foo = (foo_caddy_t*)arg;
    fprintf(stderr, "Deleting event\n");
    opal_event_del(&foo->write_event);
    PRRTE_RELEASE(foo);
    fprintf(stderr, "Write event fired\n");
    fd_written = true; /* This needs a lock around it if you are reading it
                        * in the main thread and changing it here XXX */
}
