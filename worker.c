#include "mysw.h"

static int worker_run_co(fdh_t *fdh, worker_t *worker);
static void *worker_main(void *arg);

int worker_create_all() {
    int rv, i;
    worker_t *worker;

    // allocate workers
    mysw.workers = calloc(mysw.opt_num_workers, sizeof(worker_t));
    if (!mysw.workers) {
        perror("create_workers: calloc");
        return MYSW_ERR;
    }

    // init fds to -1
    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        worker->epollfd = -1;
    }

    // init each worker
    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        worker->num = i;
        worker->epollfd = epoll_create(1);
        if (worker->epollfd < 0) {
            perror("create_workers: epoll_create");
            return MYSW_ERR;
        }
        worker->events = calloc(mysw.opt_num_epoll_events, sizeof(struct epoll_event));
        if (!worker->events) {
            perror("create_workers: calloc");
            return MYSW_ERR;
        }
        if_err_return(rv, pthread_create(&worker->thread.thread, NULL, worker_main, worker));
        worker->thread.created = 1;
    }

    return MYSW_OK;
}

int worker_join_all() {
    int rv, i;
    worker_t *worker;

    // join worker threads
    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        if (worker->thread.created) {
            if_err_return(rv, pthread_join(worker->thread.thread, NULL));
        }
    }

    return MYSW_OK;
}

int worker_free_all() {
    int i;
    worker_t *worker;

    // bail if not allocated
    if (!mysw.workers) {
        return MYSW_OK;
    }

    // free each worker
    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        if (worker->epollfd >= 0) {
            close(worker->epollfd);
        }
        if (worker->events) {
            free(worker->events);
        }
    }

    free(mysw.workers);

    return MYSW_OK;
}

static void *worker_main(void *arg) {
    int nfds, i;
    worker_t *worker;
    struct epoll_event *event;
    fdh_t *fdh;

    worker = (worker_t*)arg;

    // create main coroutine
    aco_thread_init(NULL);
    worker->main_co = aco_create(NULL, NULL, 0, NULL, NULL);

    // loop until done
    while (!mysw.done) {

        // wait on fds
        nfds = epoll_wait(worker->epollfd, worker->events, mysw.opt_num_epoll_events, mysw.opt_worker_epoll_timeout_ms);
        if (nfds < 0) {
            // epoll error
            if (errno == EINTR) continue;
            perror("worker_main: epoll_wait");
            break;
        } else if (nfds == 0) {
            // no activity
            continue;
        }

        // process events
        for (i = 0; i < nfds; ++i) {
            event = worker->events + i;
            fdh = (fdh_t*)event->data.ptr;
            fdh->fdo->event = event;
            worker_run_co(fdh, worker);
        }
    }

    // destroy main coroutine
    aco_destroy(worker->main_co);

    return NULL;
}

static int worker_run_co(fdh_t *fdh, worker_t *worker) {
    fdo_t *fdo;

    fdo = fdh->fdo;

    // allocate coroutine if needed
    // TODO libaco errors
    if (!fdo->co) {
        fdo->co_stack = aco_share_stack_new(0);
        fdo->co = aco_create(worker->main_co, fdo->co_stack, 0, fdo->co_func, fdo->co_arg);
    }

    // resume co
    aco_resume(fdo->co);

    return MYSW_OK;
}

