#include "mysw.h"

static void *worker_main(void *arg);

int worker_init(worker_t *worker, proxy_t *proxy) {
    worker->proxy = proxy;
    worker->events = calloc(opt_epoll_max_events, sizeof(struct epoll_event));
    return MYSW_OK;
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
    return MYSW_OK;
}

int worker_deinit(worker_t *worker) {
    if (worker->events) free(worker->events);
    return MYSW_OK;
}

int worker_accept_conn(fdh_t *fdh) {
    proxy_t *proxy;
    client_t *client;
    int connfd, sock_flags;

    proxy = fdh->u.proxy;

    /* Accept client conn */
    if ((connfd = accept(proxy->fdh_listen.fd, NULL, NULL)) < 0) {
        perror("worker_accept_conn: accept");
        return MYSW_ERR;
    }

    /* Set non-blocking mode */
    if ((sock_flags = fcntl(connfd, F_GETFL, 0)) < 0 || fcntl(connfd, F_SETFL, sock_flags | O_NONBLOCK) < 0) {
        perror("worker_accept_conn: fcntl");
        close(connfd);
        return MYSW_ERR;
    }

    /* Create client */
    if (client_new(proxy, connfd, &client) != MYSW_OK) {
        close(connfd);
        return MYSW_ERR;
    }

    /* Watch client socket */
    if (fdh_watch(&client->fdh_conn) != MYSW_OK) {
        client->fdh_conn.destroy = 1;
        return MYSW_ERR;
    }

    return MYSW_OK;
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

            /* Invoke read/write handler (usually fdh_read_write) */
            if (fdh->fn_read_write) {
                (fdh->fn_read_write)(fdh);
            }

            /* Invoke type-specific handler (client_process, server_process, etc) */
            (fdh->fn_process)(fdh);

            if (fdh->destroy) {
                /* Unwatch and destroy */
                fdh_unwatch(fdh);
                (fdh->fn_destroy)(fdh);
            } else if (fdh->epoll_flags != 0) { /* TODO test for EPOLLIN/OUT */
                /* Rewatch */
                fdh_rewatch(fdh);
            } else {
                /* Unwatch */
                fdh_unwatch(fdh);
            }
        }
    }

    free(events);
    return NULL;
}
