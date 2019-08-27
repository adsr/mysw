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
    fdh_init(&targeter->fdh_event, NULL, proxy->fdpoll, targeter, &targeter->spinlock, FDH_TYPE_EVENT, efd, targeter_process);
    fdh_ensure_watched(&targeter->fdh_event);

    *out_targeter = targeter;
    return MYSW_OK;
}

int targeter_process(fdh_t *fdh) {
    targeter_t *targeter;
    client_t *client;
    cmd_t *cmd;

    targeter = fdh->udata;

    if (!targeter->client_queue) {
        return MYSW_OK;
    }

    /* TODO lock */
    client = targeter->client_queue;
    LL_DELETE2(targeter->client_queue, client, next_in_targeter);

    /* TODO actual targeting */
    cmd = &client->cmd;
    client->target_server = server_a;
    server_a->target_client = client;

    server_wakeup(server_a);

    return MYSW_OK;
}

int targeter_queue_client(targeter_t *targeter, client_t *client) {
    /* TODO lock */
    LL_APPEND2(targeter->client_queue, client, next_in_targeter);
    return MYSW_OK;
}

int targeter_wakeup(targeter_t *targeter) {
    uint64_t i;
    i = 1;
    return (write(targeter->fdh_event.fd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}
