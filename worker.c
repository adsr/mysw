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

    proxy = fdh->udata;

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
    if (fdh_ensure_watched(&client->fdh_socket) != MYSW_OK) {
        /* TODO destroy */
        return MYSW_ERR;
    }

    return MYSW_OK;
}

static void *worker_main(void *arg) {
    worker_t *worker;
    worker = (worker_t *)arg;
    fdpoll_event_loop(worker->proxy->fdpoll);
    return NULL;
}
