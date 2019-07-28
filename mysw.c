#include "mysw.h"

char* opt_addr = NULL;
int opt_port = 3306;
int opt_backlog = 16;
int opt_num_threads = 16;
int opt_epoll_max_events = 256;
int opt_epoll_timeout_ms = 1000;
int opt_read_size = 256;

int main(int argc, char **argv) {
    int i, exit_code;
    struct sockaddr_in addr;
    proxy_t *proxy;

    (void)argc;
    (void)argv;

    exit_code = 1;

    /* Seed RNG */
    srand(time(NULL));

    /* Init proxy global */
    proxy = calloc(1, sizeof(proxy_t));
    proxy->fdh_listen.proxy = proxy;
    proxy->fdh_listen.type = FDH_TYPE_PROXY;
    proxy->fdh_listen.skip_read_write = 1; /* Do not read/write on listen socket */
    proxy->fdh_listen.u.proxy = proxy;
    proxy->fdh_listen.fd = -1;
    proxy->fdh_listen.epoll_flags = EPOLLIN;
    proxy->epfd = -1;
    proxy->workers = calloc(opt_num_threads, sizeof(worker_t));

    /* Create epoll fd */
    if ((proxy->epfd = epoll_create(1)) < 0) {
        perror("epoll_create");
        goto main_error;
    }

    /* Create worker threads */
    /* TODO signal handling thread */
    for (i = 0; i < opt_num_threads; ++i) {
        worker_init(&proxy->workers[i], proxy);
        if (worker_spawn(&proxy->workers[i]) != 0) {
            fprintf(stderr, "worker_spawn: failed\n");
            goto main_error;
        }
    }

    /* Create listener socket */
    if ((proxy->fdh_listen.fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        goto main_error;
    }

    /* Bind to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = opt_addr ? inet_addr(opt_addr) : INADDR_ANY;
    addr.sin_port = htons(opt_port);
    if (bind(proxy->fdh_listen.fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto main_error;
    }

    /* Start listening */
    if (listen(proxy->fdh_listen.fd, opt_backlog) < 0) {
        perror("listen");
        goto main_error;
    }

    /* Poll listener socket */
    if (fdh_watch(&proxy->fdh_listen) < 0) {
        goto main_error;
    }

    exit_code = 0;
    goto main_done;

main_error:
    proxy->done = 1;

main_done:
    /* TODO destroy clients, servers */
    for (i = 0; i < opt_num_threads; ++i) {
        worker_join(&proxy->workers[i]);
        worker_deinit(&proxy->workers[i]);
    }
    if (proxy->epfd != -1) close(proxy->epfd);
    if (proxy->fdh_listen.fd != -1) close(proxy->fdh_listen.fd);
    if (proxy->workers) free(proxy->workers);
    free(proxy);

    return exit_code;
}
