#include "mysw.h"

static int worker_accept_conn(worker_t *worker);
static void *worker_main(void *arg);

int worker_init(worker_t *worker, proxy_t *proxy) {
    worker->proxy = proxy;
    worker->events = calloc(opt_epoll_max_events, sizeof(struct epoll_event));
    return 0;
}

int worker_spawn(worker_t *worker) {
    int rv;
    if ((rv = pthread_create(&worker->thread, NULL, worker_main, worker)) == 0) {
        worker->spawned = 1;
    }
    return rv;
}

int worker_join(worker_t *worker) {
    if (worker->spawned) pthread_join(worker->thread, NULL);
    return 0;
}

int worker_deinit(worker_t *worker) {
    if (worker->events) free(worker->events);
    return 0;
}

static int worker_accept_conn(worker_t *worker) {
    int connfd, sock_flags;

    /* Accept client conn */
    if ((connfd = accept(worker->proxy->sockfd, NULL, NULL)) < 0) {
        perror("worker_accept_conn: accept");
        return 1;
    }

    /* Set non-blocking mode */
    if ((sock_flags = fcntl(connfd, F_GETFL, 0)) < 0 || fcntl(connfd, F_SETFL, sock_flags | O_NONBLOCK) < 0) {
        perror("worker_accept_conn: fcntl");
        close(connfd);
        return 1;
    }

    /* Create client */
    return client_new(worker, connfd, NULL);
}

static void *worker_main(void *arg) {
    int nfds, i;
    struct epoll_event *events;
    fdh_t *fdh;
    worker_t *worker;
    proxy_t *proxy;

    worker = (worker_t *)arg;
    proxy = worker->proxy;
    events = calloc(opt_epoll_max_events, sizeof(struct epoll_event));

    while (!proxy->done) {
        /* Wait for events */
        nfds = epoll_wait(proxy->epfd, events, opt_epoll_max_events, opt_epoll_timeout_ms);
        if (nfds < 0) {
            if (errno != EINTR) perror("worker_main: epoll_wait");
            continue;
        }

        /* Handle events */
        for (i = 0; i < nfds; ++i) {
            fdh = (fdh_t *)events[i].data.ptr;

            if (fdh->skip_read_write) {
                /* Skip generic io */
            } else {
                /* Do generic io */
                fdh_read_write(fdh);
            }

            /* Call owner-specific handler */
            switch (fdh->type) {
                case FDH_TYPE_PROXY:  worker_accept_conn(worker);    break;
                case FDH_TYPE_CLIENT: client_process(fdh->u.client); break;
                /* case FDH_TYPE_SERVER: server_process(fdh->u.server); break; */
                default: fprintf(stderr, "worker_main: Unrecognized fdh type %d\n", fdh->type);
            }

            /* Rewatch/unwatch fd */
            if (fdh->epoll_flags != 0) {
                fdh_rewatch(fdh);
            } else {
                fdh_unwatch(fdh);
            }
        }
    }

    free(events);
    return NULL;
}
