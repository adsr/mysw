#include "mysw.h"

mysw_t mysw;

int main(int argc, char **argv) {
    int rv;

    (void)argc;
    (void)argv;

    // init globals
    // TODO flags
    memset(&mysw, 0, sizeof(mysw));
    mysw.opt_port = 3306;
    mysw.opt_backlog = 32;
    mysw.opt_num_workers = 1;
    mysw.opt_num_acceptors = 2;
    mysw.opt_num_epoll_events = 128;
    mysw.opt_max_num_clients = 128;
    mysw.opt_worker_epoll_timeout_ms = 1000;
    mysw.opt_acceptor_select_timeout_s = 1;
    mysw.done_pipe[0] = -1;
    mysw.done_pipe[1] = -1;
    mysw.listenfd = -1;

    pool_t *pool;
    pool = calloc(1, sizeof(pool_t));
    pool->name = strdup("pool_a");
    pool->num_servers = 2;
    pool->server_tpl.host = "localhost";
    pool->server_tpl.ip4 = "127.0.0.1"; // TODO getaddrinfo
    pool->server_tpl.port = 3306;
    pool->server_tpl.db_name = "test";
    HASH_ADD_KEYPTR(hh, mysw.pool_map, pool->name, strlen(pool->name), pool);

    rv = 0;
    do {
        if_err_break(rv, signal_create_thread());
        if_err_break(rv, worker_create_all());
        // if_err_break(rv, targeter_create_all());
        if_err_break(rv, client_create_all());
        if_err_break(rv, server_create_all());
        if_err_break(rv, listener_create());
        if_err_break(rv, acceptor_create_all());
    } while (0);

    rv |= acceptor_join_all();
    // rv |= targeter_join_all();
    rv |= worker_join_all();
    rv |= signal_join_thread();

    acceptor_free_all();
    listener_free();
    server_free_all();
    client_free_all();
    // targeter_free_all();
    worker_free_all();

    return rv;
}
