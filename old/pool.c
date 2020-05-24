#include "mysw.h"

int pool_new(proxy_t *proxy, char *name, pool_t **out_pool) {
    pool_t *pool;
    int efd;

    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        return MYSW_ERR;
    }

    pool = calloc(1, sizeof(pool_t));
    pool->proxy = proxy;
    pool->name = strdup(name);
    pthread_spin_init(&pool->spinlock_server_lists, PTHREAD_PROCESS_PRIVATE);
    pthread_spin_init(&pool->spinlock_client_queue, PTHREAD_PROCESS_PRIVATE);

    fdo_init(&pool->fdo, proxy->fdpoll, pool, &pool->state, NULL, -1, efd, -1, pool_process);

    /* Add to pool_map */
    pthread_spin_lock(proxy->spinlock_pool_map);
    HASH_ADD_KEYPTR(hh, proxy->pool_map, pool->name, strlen(pool->name), pool);
    pthread_spin_unlock(proxy->spinlock_pool_map);

    *out_pool = pool;
    return MYSW_OK;
}

int pool_process(fdh_t *fdh) {
    pool_t *pool;
    client_t *client;
    server_t *server;

    pool = fdh->fdo->udata;

    /* Reserve a free server */
    pool_server_reserve(pool, &server);

    if (!server) {
        /* TODO no free servers left... send error? keep in queue? */
    }

    /* Shift head of client_queue */
    pthread_spin_lock(&pool->spinlock_client_queue);
    client = pool->client_queue; /* TODO assert client->target_pool == pool */
    LL_DELETE2(pool->client_queue, client, next_in_pool);
    pthread_spin_unlock(&pool->spinlock_client_queue);

    /* Bind client and server */
    client->target_server = server;
    server->target_client = client;

    /* Wake up server */
    server_wakeup(server);

    return MYSW_OK;
}

int pool_find(proxy_t *proxy, char *name, size_t name_len, pool_t **out_pool) {
    pool_t *pool;
    pool = NULL;

    /* Find in pool_map */
    pthread_spin_lock(proxy->spinlock_pool_map);
    HASH_FIND(hh, proxy->pool_map, name, name_len, pool);
    pthread_spin_unlock(proxy->spinlock_pool_map);

    *out_pool = pool;
    return MYSW_OK;
}

int pool_fill(pool_t *pool, char *host, int port, char *dbname, int n) {
    server_t *server;
    int i;
    for (i = 0; i < n; i++) {
        server_new(pool, host, port, dbname, &server);
        pool_server_move_to_dead(pool, server);
    }
    return MYSW_OK;
}

int pool_server_reserve(pool_t *pool, server_t **out_server) {
    server_t *server;

    pthread_spin_lock(&pool->spinlock_server_lists);
    server = pool->servers_free;
    if (server) {
        pool_server_remove_from_list(pool, server);
        LL_APPEND2(pool->servers_reserved, server, next_in_reserved);
        server->in_reserved = 1;
    }
    *out_server = server;
    pthread_spin_unlock(&pool->spinlock_server_lists);

    return MYSW_OK;
}

int pool_server_move_to_dead(pool_t *pool, server_t *server) {
    pthread_spin_lock(&pool->spinlock_server_lists);
    pool_server_remove_from_list(pool, server);
    LL_APPEND2(pool->servers_dead, server, next_in_dead);
    server->in_dead = 1;
    pthread_spin_unlock(&pool->spinlock_server_lists);
    return MYSW_OK;
}

int pool_server_move_to_free(pool_t *pool, server_t *server) {
    pthread_spin_lock(&pool->spinlock_server_lists);
    pool_server_remove_from_list(pool, server);
    LL_APPEND2(pool->servers_free, server, next_in_free);
    server->in_free = 1;
    pthread_spin_unlock(&pool->spinlock_server_lists);
    return MYSW_OK;
}

int pool_server_move_to_reserved(pool_t *pool, server_t *server) {
    pthread_spin_lock(&pool->spinlock_server_lists);
    pool_server_remove_from_list(pool, server);
    LL_APPEND2(pool->servers_reserved, server, next_in_reserved);
    server->in_reserved = 1;
    pthread_spin_unlock(&pool->spinlock_server_lists);
    return MYSW_OK;
}

int pool_server_remove_from_list(pool_t *pool, server_t *server) {
    if (server->in_dead)     LL_DELETE2(pool->servers_dead,     server, next_in_dead);
    if (server->in_free)     LL_DELETE2(pool->servers_free,     server, next_in_free);
    if (server->in_reserved) LL_DELETE2(pool->servers_reserved, server, next_in_reserved);
    server->in_dead = 0;
    server->in_free = 0;
    server->in_reserved = 0;
    return MYSW_OK;
}


int pool_queue_client(pool_t *pool, client_t *client) {
    pthread_spin_lock(&pool->spinlock_client_queue);
    LL_APPEND2(pool->client_queue, client, next_in_pool);
    pthread_spin_unlock(&pool->spinlock_client_queue);
    return MYSW_ERR;
}

int pool_wakeup(pool_t *pool) {
    (void)pool;
    return MYSW_ERR;
}

int pool_destroy(pool_t *pool) {
    client_t *client, *client_tmp;
    server_t *server, *server_tmp;

    /* Empty server lists */
    pthread_spin_lock(&pool->spinlock_server_lists);
    LL_FOREACH_SAFE2(pool->servers_dead, server, server_tmp, next_in_dead) {
        LL_DELETE2(pool->servers_dead, server, next_in_dead);
        server->in_dead = 0;
    }
    LL_FOREACH_SAFE2(pool->servers_free, server, server_tmp, next_in_free) {
        LL_DELETE2(pool->servers_free, server, next_in_free);
        server->in_free = 0;
    }
    LL_FOREACH_SAFE2(pool->servers_reserved, server, server_tmp, next_in_reserved) {
        LL_DELETE2(pool->servers_reserved, server, next_in_reserved);
        server->in_reserved = 0;
    }
    pthread_spin_unlock(&pool->spinlock_server_lists);

    /* Empty client_queue */
    pthread_spin_lock(&pool->spinlock_client_queue);
    LL_FOREACH_SAFE2(pool->client_queue, client, client_tmp, next_in_pool) {
        LL_DELETE2(pool->client_queue, client, next_in_pool);
    }
    pthread_spin_unlock(&pool->spinlock_client_queue);

    /* Delete self from pool_map */
    pthread_spin_lock(pool->proxy->spinlock_pool_map);
    HASH_DEL(pool->proxy->pool_map, pool);
    pthread_spin_unlock(pool->proxy->spinlock_pool_map);

    pthread_spin_destroy(&pool->spinlock_server_lists);
    pthread_spin_destroy(&pool->spinlock_client_queue);

    close(pool->fdo.event_in.fd);
    fdo_deinit(&pool->fdo);

    free(pool->name);
    free(pool);
    return MYSW_OK;
}
