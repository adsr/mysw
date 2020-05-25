#include "mysw.h"

static int create_signal_thread();
static int create_workers();
static int join_worker_threads();
static int join_signal_thread();
static int free_workers();

mysw_t mysw;

int main(int argc, char **argv) {
    int rv;

    (void)argc;
    (void)argv;

    memset(&mysw, 0, sizeof(mysw));
    mysw.done_pipe[0] = -1;
    mysw.done_pipe[1] = -1;
    rv = 0;

    do {
        if_err_break(rv, create_signal_thread());
        if_err_break(rv, create_workers());
        // if_err_break(rv, create_targeters());
        // if_err_break(rv, create_clients());
        // if_err_break(rv, create_backends());
        // if_err_break(rv, create_listener());
        // if_err_break(rv, create_acceptors());
    } while (0);

    // rv |= join_acceptor_threads();
    // rv |= join_targeter_threads();
    rv |= join_worker_threads();
    rv |= join_signal_thread();

    // free_acceptors();
    // free_listener();
    // free_backends();
    // free_clients();
    // free_targeters();
    free_workers();

    return rv;
}

static int create_signal_thread() {
    int rv;
    if_err_return(
        rv,
        pthread_create(&mysw.signal_thread.thread, NULL, signal_main, NULL)
    );
    mysw.signal_thread.created = 1;
    if_err_return(rv, signal_block_all());
    return MYSW_OK;
}

static int create_workers() {
    int rv, i;
    worker_t *worker;

    mysw.workers = calloc(mysw.opt_num_workers, sizeof(worker_t));
    if (!mysw.workers) {
        perror("create_workers: calloc");
        return MYSW_ERR;
    }
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

static int join_worker_threads() {
    int rv, i;
    worker_t *worker;

    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        if (worker->thread.created) {
            if_err_return(rv, pthread_join(worker->thread.thread, NULL));
        }
    }
    return MYSW_OK;
}

static int join_signal_thread() {
    int rv;

    if (mysw.signal_thread.created) {
        if_err_return(rv, pthread_join(mysw.signal_thread.thread, NULL));
    }
    return MYSW_OK;
}

static int free_workers() {
    int i;
    worker_t *worker;

    if (!mysw.workers) {
        return MYSW_OK;
    }

    for (i = 0; i < mysw.opt_num_workers; ++i) {
        worker = mysw.workers + i;
        if (worker->epollfd >= 0) close(worker->epollfd);
        if (worker->events) free(worker->events);
    }

    free(mysw.workers);

    return MYSW_OK;
}