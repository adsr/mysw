#include "mysw.h"

static int worker_resume_fdo(fdo_t *fdo, worker_t *worker);

void *worker_main(void *arg) {
    worker_t *worker;
    struct epoll_event *event;
    fdo_t *fdo;
    int nfds, i;

    worker = (worker_t*)arg;

    // create main coroutine
    // TODO libaco errors
    aco_thread_init(NULL);
    worker->main_co = aco_create(NULL, NULL, 0, NULL, NULL);

    // loop until done
    while (!mysw.done) {

        // wait on fds
        nfds = epoll_wait(worker->epollfd, worker->events, mysw.opt_num_epoll_events, 1000);
        if (nfds < 0) {
            // epoll error
            if (errno == EINTR) continue;
            perror("worker_main: epoll_wait");
            break;
        } else if (nfds == 0) {
            // no activity
            continue;
        }

        // process owner of each event
        for (i = 0; i < nfds; ++i) {
            event = worker->events + i;
            fdo = (fdo_t*)event->data.ptr;
            fdo->last_event = event;
            worker_resume_fdo(fdo, worker);
        }
    }

    // destroy main co
    aco_destroy(worker->main_co);

    return NULL;
}

static int worker_resume_fdo(fdo_t *fdo, worker_t *worker) {
    void (*co_func)();
    void *co_arg;

    switch (fdo->type) {
        case MYSW_FDO_TYPE_CLIENT:
            co_func = client_handle;
            co_arg = fdo->owner.client;
            break;
        case MYSW_FDO_TYPE_BACKEND:
            co_func = backend_handle;
            co_arg = fdo->owner.backend;
            break;
        default:
            fprintf(stderr, "worker_resume_fdo: Unrecognized fdo type %d\n", fdo->type);
            return MYSW_ERR;
    }

    // allocate co if needed
    // TODO preallocate, needs aco_reset
    // TODO libaco errors
    if (!fdo->co) {
        fdo->stack = aco_share_stack_new(0);
        fdo->co = aco_create(worker->main_co, fdo->stack, 0, co_func, co_arg);
    }

    // resume co
    aco_resume(fdo->co);

    // destroy co if dead
    if (fdo->co_dead) {
        aco_destroy(fdo->co);
        aco_share_stack_destroy(fdo->stack);
        fdo->co = NULL;
        fdo->stack = NULL;
    }

    return MYSW_OK;
}
