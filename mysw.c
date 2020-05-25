#include "mysw.h"

// grep ^static mysw.c | sed 's| {|;|g'
static int create_acceptors();
static int create_clients();
static int create_listener();
static int create_signal_thread();
static int create_workers();
static int free_acceptors();
static int free_clients();
static int free_listener();
static int free_workers();
static int join_acceptor_threads();
static int join_signal_thread();
static int join_worker_threads();

mysw_t mysw;

int main(int argc, char **argv) {
    int rv;

    (void)argc;
    (void)argv;

    // init globals
    memset(&mysw, 0, sizeof(mysw));
    mysw.opt_port = 3306;
    mysw.opt_backlog = 32;
    mysw.opt_num_workers = 2;
    mysw.opt_num_acceptors = 2;
    mysw.opt_num_epoll_events = 128;
    mysw.opt_max_num_clients = 128;
    mysw.opt_worker_epoll_timeout_ms = 1000;
    mysw.opt_acceptor_select_timeout_s = 1;
    mysw.done_pipe[0] = -1;
    mysw.done_pipe[1] = -1;
    mysw.listenfd = -1;
    rv = 0;

    do {
        if_err_break(rv, create_signal_thread());
        if_err_break(rv, create_workers());
        // if_err_break(rv, create_targeters());
        if_err_break(rv, create_clients());
        // if_err_break(rv, create_backends());
        if_err_break(rv, create_listener());
        if_err_break(rv, create_acceptors());
    } while (0);

    rv |= join_acceptor_threads();
    // rv |= join_targeter_threads();
    rv |= join_worker_threads();
    rv |= join_signal_thread();

    free_acceptors();
    free_listener();
    // free_backends();
    free_clients();
    // free_targeters();
    free_workers();

    return rv;
}

static int create_signal_thread() {
    int rv;

    // create signal thread
    if_err_return(rv, pthread_create(&mysw.signal_thread.thread, NULL, signal_main, NULL));
    mysw.signal_thread.created = 1;

    // block all signals for all other threads
    if_err_return(rv, signal_block_all());

    return MYSW_OK;
}

static int create_workers() {
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

static int create_clients() {
    int rv, i;
    client_t *client;
    struct epoll_event event;

    // allocate clients
    mysw.clients = calloc(mysw.opt_max_num_clients, sizeof(client_t));
    mysw.clients_unused = mysw.clients;
    if ((rv = pthread_spin_init(&mysw.clients_lock.spinlock, PTHREAD_PROCESS_SHARED)) != 0) {
        errno = rv;
        perror("create_clients: pthread_spin_init");
        return MYSW_ERR;
    }
    mysw.clients_lock.created = 1;

    // init fds to -1
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;
        client->fdh_socket.fd = -1;
        client->fdh_event.fd = -1;
        client->fdh_timer.fd = -1;
    }

    // init each client
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;
        client->num = i;

        // init next_unused
        if (i + 1 < mysw.opt_max_num_clients) {
            client->next_unused = mysw.clients + i + 1;
        }

        // init fdo
        client->fdo.type = MYSW_FDO_TYPE_CLIENT;
        client->fdo.owner.client = client;
        client->fdh_socket.fdo = &client->fdo;
        client->fdh_event.fdo = &client->fdo;
        client->fdh_timer.fdo = &client->fdo;

        // assign worker
        client->worker = mysw.workers + (client->num % mysw.opt_num_workers);

        // create eventfd
        if ((client->fdh_event.fd = eventfd(0, EFD_NONBLOCK)) < 0) {
            perror("create_clients: eventfd");
            return MYSW_ERR;
        }

        // create timerfd
        if ((client->fdh_timer.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
            perror("create_clients: timerfd_create");
            return MYSW_ERR;
        }

        // add eventfd to worker epoll
        event.events = EPOLLIN;
        event.data.ptr = &client->fdh_event;
        if (epoll_ctl(client->worker->epollfd, EPOLL_CTL_ADD, client->fdh_event.fd, &event) < 0) {
            perror("create_clients: epoll_ctl");
            return MYSW_ERR;
        }

        // add timertfd to worker epoll
        event.events = EPOLLIN;
        event.data.ptr = &client->fdh_timer;
        if (epoll_ctl(client->worker->epollfd, EPOLL_CTL_ADD, client->fdh_timer.fd, &event) < 0) {
            perror("create_clients: epoll_ctl");
            return MYSW_ERR;
        }
    }

    return MYSW_OK;
}

static int create_listener() {
    struct sockaddr_in addr;
    int optval;

    // create socket
    if ((mysw.listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("create_listener: socket");
        return MYSW_ERR;
    }

    // set SO_REUSEPORT
    optval = 1;
    if ((setsockopt(mysw.listenfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) < 0) {
        perror("create_listener: setsockopt");
        return MYSW_ERR;
    }

    // bind to port
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = mysw.opt_addr ? inet_addr(mysw.opt_addr) : INADDR_ANY;
    addr.sin_port = htons(mysw.opt_port);
    if (bind(mysw.listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("create_listener: bind");
        return MYSW_ERR;
    }

    // listen
    if (listen(mysw.listenfd, mysw.opt_backlog) < 0) {
        perror("create_listener: listen");
        return MYSW_ERR;
    }

    return MYSW_OK;
}

static int free_listener() {
    if (mysw.listenfd >= 0) {
        close(mysw.listenfd);
    }
    return MYSW_OK;
}

static int create_acceptors() {
    int rv, i;
    acceptor_t *acceptor;

    // allocate acceptors
    mysw.acceptors = calloc(mysw.opt_num_acceptors, sizeof(acceptor_t));
    if (!mysw.acceptors) {
        perror("create_acceptors: calloc");
        return MYSW_ERR;
    }

    // create acceptor threads
    for (i = 0; i < mysw.opt_num_acceptors; ++i) {
        acceptor = mysw.acceptors + i;
        acceptor->num = i;
        if_err_return(rv, pthread_create(&acceptor->thread.thread, NULL, acceptor_main, acceptor));
        acceptor->thread.created = 1;
    }

    return MYSW_OK;
}

static int join_worker_threads() {
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

static int join_signal_thread() {
    int rv;

    // join signal thread
    if (mysw.signal_thread.created) {
        if_err_return(rv, pthread_join(mysw.signal_thread.thread, NULL));
    }

    return MYSW_OK;
}

static int join_acceptor_threads() {
    int rv, i;
    acceptor_t *acceptor;

    // join acceptor threads
    for (i = 0; i < mysw.opt_num_acceptors; ++i) {
        acceptor = mysw.acceptors + i;
        if (acceptor->thread.created) {
            if_err_return(rv, pthread_join(acceptor->thread.thread, NULL));
        }
    }

    return MYSW_OK;
}

static int free_workers() {
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

static int free_clients() {
    int i;
    client_t *client;

    // bail if not allocated
    if (!mysw.clients) {
        return MYSW_OK;
    }

    // free lock
    if (mysw.clients_lock.created) {
        pthread_spin_destroy(&mysw.clients_lock.spinlock);
    }

    // free each client
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;
        if (client->fdh_socket.fd >= 0) {
            epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_socket.fd, NULL);
            close(client->fdh_socket.fd);
        }
        if (client->fdh_event.fd >= 0) {
            epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_event.fd, NULL);
            close(client->fdh_event.fd);
        }
        if (client->fdh_timer.fd >= 0) {
            epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_timer.fd, NULL);
            close(client->fdh_timer.fd);
        }
    }

    free(mysw.clients);

    return MYSW_OK;
}

static int free_acceptors() {
    // bail if not allocated
    if (!mysw.acceptors) {
        return MYSW_OK;
    }

    free(mysw.acceptors);

    return MYSW_OK;
}