#include "mysw.h"

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
        if_err_break(rv, signal_create_thread());
        if_err_break(rv, worker_create_all());
        // if_err_break(rv, targeter_create_all());
        if_err_break(rv, client_create_all());
        // if_err_break(rv, backend_create_all());
        if_err_break(rv, listener_create());
        if_err_break(rv, acceptor_create_all());
    } while (0);

    rv |= acceptor_join_all();
    // rv |= targeter_join_all();
    rv |= worker_join_all();
    rv |= signal_join_thread();

    acceptor_free_all();
    listener_free();
    // backend_free_all();
    client_free_all();
    // targeter_free_all();
    worker_free_all();

    return rv;
}
