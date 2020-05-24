#include "mysw.h"

int targeter_new(proxy_t *proxy, targeter_t **out_targeter) {
    targeter_t *targeter;
    int efd;

    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        return MYSW_ERR;
    }

    targeter = calloc(1, sizeof(targeter_t));
    targeter->proxy = proxy;
    pthread_spin_init(&targeter->spinlock, PTHREAD_PROCESS_PRIVATE);

    fdo_init(&targeter->fdo, proxy->fdpoll, targeter, NULL, &targeter->spinlock, -1, efd, -1, targeter_process);

    *out_targeter = targeter;
    return MYSW_OK;
}

int targeter_spawn_targlet_threads(targeter_t *targeter) {
    (void)targeter;
    return MYSW_ERR;
}

int targeter_process(fdh_t *fdh) {
    targeter_t *targeter;
    client_t *client, *client_tmp;

    targeter = fdh->fdo->udata;

    LL_FOREACH_SAFE2(targeter->client_queue, client, client_tmp, next_in_targeter) {
        if (client->target_pool_name) {
            targeter_pool_client(targeter, client);
        } else {
            targeter_target_client(targeter, client);
        }
        LL_DELETE2(targeter->client_queue, client, next_in_targeter);
    }

    return MYSW_OK;
}

int targeter_target_client(targeter_t *targeter, client_t *client) {
    targlet_t *targlet;

    if (!targeter->targlets_free) {
        /* TODO send error to client (no free targlets available) */
        return MYSW_ERR;
    }
    targlet = targeter->targlets_free;

    LL_DELETE2(targeter->targlets_free, targlet, next_in_free);
    targlet->client = client;
    HASH_ADD_KEYPTR(hh_in_reserved, targeter->targlets_reserved, targlet, sizeof(targlet_t *), targlet);

    targlet_wakeup(targlet);

    return MYSW_OK;
}

int targeter_pool_client(targeter_t *targeter, client_t *client) {
    pool_t *pool;

    pool_find(targeter->proxy, client->target_pool_name, strlen(client->target_pool_name), &pool);
    if (!pool) {
        /* TODO send error to client (pool not found) */
        return MYSW_ERR;
    }

    pool_queue_client(pool, client);
    pool_wakeup(pool);

    return MYSW_OK;
}

int targeter_queue_client_from_targlet(targeter_t *targeter, client_t *client, targlet_t *targlet) {
    pthread_spin_lock(&targeter->spinlock);

    HASH_DELETE(hh_in_reserved, targeter->targlets_reserved, targlet);
    targlet->client = NULL;
    LL_PREPEND2(targeter->targlets_free, targlet, next_in_free);

    LL_APPEND2(targeter->client_queue, client, next_in_targeter); /* TODO make this doubly-linked for efficient append and shift */

    pthread_spin_unlock(&targeter->spinlock);
    return MYSW_OK;
}

int targeter_queue_client(targeter_t *targeter, client_t *client) {
    pthread_spin_lock(&targeter->spinlock);

    LL_APPEND2(targeter->client_queue, client, next_in_targeter);

    pthread_spin_unlock(&targeter->spinlock);
    return MYSW_OK;
}

int targlet_wakeup(targlet_t *targlet) {
    uint64_t i;
    return (write(targlet->efd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}

/*
static void *targlet_main(void *arg) {
    |* TODO
    while (!targlet->targeter->proxy->fdpoll->done) {
        poll eventfd
        invoke script with client.cmd as param
        set targlet->client->target_pool_name
        targeter_queue_client_from_targlet
    } *|
    return NULL;
}

*/