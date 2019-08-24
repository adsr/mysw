#include "mysw.h"

int targeter_new(proxy_t *proxy, targeter_t **out_targeter) {
    targeter_t *targeter;
    int efd;

    /* Make event fd */
    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        perror("targeter_new: eventfd");
        return MYSW_ERR;
    }

    targeter = calloc(1, sizeof(targeter_t));
    pthread_spin_init(&targeter->spinlock, PTHREAD_PROCESS_PRIVATE);

    /* Init application event handle (internal eventfd) */
    fdh_init(&targeter->fdh_event, proxy->fdpoll, FDH_TYPE_TARGETER, targeter, efd, fdh_read_u64, targeter_process);
    fdh_set_epoll_flags(&targeter->fdh_event, EPOLLIN);
    fdh_ensure_watched(&targeter->fdh_event);

    *out_targeter = targeter;
    return MYSW_OK;
}

int targeter_process(fdh_t *fdh) {
    targeter_t *targeter;
    client_t *client;

    targeter = fdh->udata;

    if (!targeter->client_queue) {
        return MYSW_OK;
    }

    client = targeter->client_queue;
    LL_DELETE2(targeter->client_queue, client, next_in_targeter);

    /* TODO actual targeting */

    client->target_server = the_server;
    the_server->target_client = client;

    server_wakeup(the_server);

    return MYSW_OK;
}

int targeter_queue_client(targeter_t *targeter, client_t *client) {
    LL_APPEND2(targeter->client_queue, client, next_in_targeter);
    return MYSW_OK;
}

int targeter_wakeup(targeter_t *targeter) {
    uint64_t i;
    i = 1;
    return (write(targeter->fdh_event.fd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}
