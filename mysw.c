#include "mysw.h"

char* opt_addr = NULL;
int opt_port = 3307;
int opt_backlog = 16;
int opt_num_threads = 1; /* TODO saner default */
int opt_epoll_max_events = 256;
int opt_epoll_timeout_ms = 1000;
int opt_read_size = 256;

static void *signal_main(void *arg);
static void signal_handle(int signum);
static int signal_block_all();

static int done_pipe[2];

int main(int argc, char **argv) {
    int i, exit_code;
    int listenfd;
    struct sockaddr_in addr;
    proxy_t *proxy;
    server_t *server;

    (void)argc;
    (void)argv;

    exit_code = 1;

    /* Seed RNG */
    srand(time(NULL));

    /* Init proxy global */
    proxy = calloc(1, sizeof(proxy_t));
    proxy->workers = calloc(opt_num_threads, sizeof(worker_t));

    /* Create signal handling thread */
    pthread_create(&proxy->signal_thread, NULL, signal_main, proxy);
    signal_block_all(); /* For all other threads, block all signals */

    /* Create listener socket */
    listenfd = -1;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("main: socket");
        goto main_error;
    }

    /* Bind to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = opt_addr ? inet_addr(opt_addr) : INADDR_ANY;
    addr.sin_port = htons(opt_port);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("main: bind");
        goto main_error;
    }

    /* Start listening */
    if (listen(listenfd, opt_backlog) < 0) {
        perror("main: listen");
        goto main_error;
    }

    /* Create event loop (epoll wrapper) */
    if (fdpoll_new(proxy, &proxy->fdpoll) != MYSW_OK) {
        goto main_error;
    }

    /* Add listener socket to event loop */
    fdh_init(&proxy->fdh_listen, proxy->fdpoll, FDH_TYPE_PROXY, proxy, listenfd, fdh_no_read_write, worker_accept_conn);
    fdh_set_epoll_flags(&proxy->fdh_listen, EPOLLIN);
    if (fdh_watch(&proxy->fdh_listen) != MYSW_OK) {
        goto main_error;
    }

    /* Connect to local mysqld */
    /* TODO let user-script do this */
    /* TODO server pools */
    server_new(proxy, "127.0.0.1", 3306, &server);
    server_process_connect(server);

    /* Create worker threads */
    for (i = 0; i < opt_num_threads; ++i) {
        worker_init(&proxy->workers[i], proxy);
        if (worker_spawn(&proxy->workers[i]) != 0) {
            fprintf(stderr, "worker_spawn: failed\n");
            goto main_error;
        }
    }

    exit_code = 0;
    goto main_done;

main_error:
    if (proxy->fdpoll) fdpoll_set_done(proxy->fdpoll);

main_done:
    /* TODO destroy clients, servers */
    for (i = 0; i < opt_num_threads; ++i) {
        worker_join(&proxy->workers[i]);
        worker_deinit(&proxy->workers[i]);
    }
    /* TODO join signal thread */
    if (listenfd != -1) close(listenfd);
    if (proxy->fdpoll) fdpoll_free(proxy->fdpoll);
    if (proxy->workers) free(proxy->workers);
    free(proxy);

    return exit_code;
}

static void *signal_main(void *arg) {
    int rv;
    int signum;
    fd_set rfds;
    struct timeval tv;
    struct sigaction sa;
    proxy_t *proxy;

    proxy = (proxy_t *)arg;

    /* Create done_pipe */
    rv = pipe(done_pipe);
    fcntl(done_pipe[1], F_SETFL, O_NONBLOCK);

    /* Install signal handler */
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = signal_handle;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* Wait for write on done_pipe from signal_handle */
    do {
        FD_ZERO(&rfds);
        FD_SET(done_pipe[0], &rfds);
        tv.tv_sec = 60;
        tv.tv_usec = 0;
        rv = select(done_pipe[0] + 1, &rfds, NULL, NULL, &tv);
    } while (rv < 1);

    /* Read pipe for fun */
    rv = read(done_pipe[0], &signum, sizeof(int));

    /* End event loop */
    fdpoll_set_done(proxy->fdpoll);
    /* TODO broadcast all conditions */

    return NULL;
}

static void signal_handle(int signum) {
    write(done_pipe[1], &signum, sizeof(int));
}

static int signal_block_all() {
    sigset_t set;
    sigfillset(&set);
    sigprocmask(SIG_BLOCK, &set, NULL);
    return 0;
}
