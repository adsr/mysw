#include "mysw.h"

int pool_new(proxy_t *proxy, char *name, pool_t **out_pool) {
    pool_t *pool;
    pool = calloc(1, sizeof(pool_t));
    pool->proxy = proxy;
    pool->name = strdup(name);
    HASH_ADD_KEYPTR(hh, proxy->pool_map, pool->name, strlen(pool->name), pool); /* TODO rwlock */
    *out_pool = pool;
    return MYSW_OK;
}

int pool_find(proxy_t *proxy, char *name, pool_t **out_pool) {
    pool_t *pool;
    pool = NULL;
    HASH_FIND_STR(proxy->pool_map, name, pool); /* TODO rwlock */
    *out_pool = pool;
    return MYSW_OK;
}

int pool_fill(pool_t *pool, char *host, int port, int n) {
    server_t *server;
    int i;
    for (i = 0; i < n; i++) {
        server_new(pool->proxy, host, port, &server);
    }
    return MYSW_OK;
}

int pool_destroy(pool_t *pool) {
    /* TODO destroy servers */
    free(pool->name);
    free(pool);
    return MYSW_OK;
}
