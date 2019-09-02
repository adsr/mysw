#include "mysw.h"

char *opt_addr = NULL;
int opt_port = 3307;
int opt_backlog = 16;
int opt_num_worker_threads = 2; /* TODO saner defaults */
int opt_epoll_max_events = 256;
int opt_epoll_timeout_ms = 1000;
int opt_read_size = 256;
pool_t *pool_a = NULL;
server_t *server_a = NULL;
server_t *server_b = NULL;

static int main_parse_args(int argc, char **argv);
static int main_init_globals(proxy_t **out_proxy);
static int main_init_signals(proxy_t *proxy);
static int main_spawn_workers(proxy_t *proxy);
static int main_join_workers(proxy_t *proxy);
static int main_listen_socket(proxy_t *proxy);
static int main_deinit(proxy_t *proxy);
static void *signal_main(void *arg);
static void signal_handle(int signum);
static int signal_block_all();

static int done_pipe[2];

int main(int argc, char **argv) {
    proxy_t *proxy;
    int rv;

    srand(time(NULL));

    main_parse_args(argc, argv);

    proxy = NULL;
    do {
        try_break(rv, main_init_globals(&proxy));
        try_break(rv, main_init_signals(proxy));
        try_break(rv, main_spawn_workers(proxy));
        try_break(rv, main_listen_socket(proxy));
    } while(0);

    if (rv != 0 && proxy->fdpoll) proxy->fdpoll->done = 1;

    main_join_workers(proxy);
    main_deinit(proxy);

    return rv == MYSW_OK ? 0 : 1;
}

static int main_parse_args(int argc, char **argv) {
    /* TODO getopt */
    (void)argc;
    (void)argv;
    return MYSW_OK;
}

static int main_init_globals(proxy_t **out_proxy) {
    proxy_t *proxy;
    int rv;

    /* Alloc proxy global */
    proxy = calloc(1, sizeof(proxy_t));

    /* Init event loop (epoll wrapper) */
    try(rv, fdpoll_new(proxy, &proxy->fdpoll));

    /* Alloc workers */
    proxy->workers = calloc(opt_num_worker_threads, sizeof(worker_t));

    /* Init targeter */
    try(rv, targeter_new(proxy, &proxy->targeter));

    /* Init spinlock */
    if ((rv = pthread_spin_init(&proxy->spinlock_pool_map_val, PTHREAD_PROCESS_PRIVATE)) != 0) {
        fprintf(stderr, "main_init_globals: pthread_spin_init: %s\n", strerror(rv));
        return MYSW_ERR;
    }
    proxy->spinlock_pool_map = &proxy->spinlock_pool_map_val;

    /* Init pools and servers */
    /* TODO user configurable pools */
    pool_new(proxy, "pool_a", &pool_a);
    pool_fill(pool_a, "127.0.0.1", 3306, "test", 10);

    /* Init targeter and targlets */
    /* TODO user configurable targeters */

    *out_proxy = proxy;
    return MYSW_OK;
}

static int main_init_signals(proxy_t *proxy) {
    int rv;

    /* Create signal handling thread */
    if ((rv = pthread_create(&proxy->signal_thread_val, NULL, signal_main, proxy)) != 0) {
        fprintf(stderr, "main_init_signals: pthread_create: %s\n", strerror(rv));
        return MYSW_ERR;
    }
    proxy->signal_thread = &proxy->signal_thread_val;

    /* For main thread and all subsequently spawned threads, block signals */
    try(rv, signal_block_all());

    return MYSW_OK;
}

static int main_spawn_workers(proxy_t *proxy) {
    int rv, i;

    /* Create worker threads */
    for (i = 0; i < opt_num_worker_threads; ++i) {
        try(rv, worker_init(&proxy->workers[i], proxy));
        try(rv, worker_spawn(&proxy->workers[i]));
    }

    return MYSW_OK;
}

static int main_join_workers(proxy_t *proxy) {
    int i;

    /* Join worker threads to main thread */
    for (i = 0; i < opt_num_worker_threads; ++i) {
        worker_join(&proxy->workers[i]);
        worker_deinit(&proxy->workers[i]);
    }

    return MYSW_OK;
}

static int main_listen_socket(proxy_t *proxy) {
    struct sockaddr_in addr;
    int listenfd, optval;

    /* Create listener socket */
    proxy->fdo.socket_in.fd = -1;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("main_listen_socket: socket");
        return MYSW_ERR;
    }
    proxy->fdo.socket_in.fd = listenfd;

    /* Set SO_REUSEPORT */
    optval = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) < 0) {
        perror("main_listen_socket: setsockopt");
        return MYSW_ERR;
    }

    /* Bind to port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = opt_addr ? inet_addr(opt_addr) : INADDR_ANY;
    addr.sin_port = htons(opt_port);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("main_listen_socket: bind");
        return MYSW_ERR;
    }

    /* Start listening */
    if (listen(listenfd, opt_backlog) < 0) {
        perror("main_listen_socket: listen");
        return MYSW_ERR;
    }

    /* Add listener socket to event loop */
    fdo_init(&proxy->fdo, proxy->fdpoll, proxy, NULL, NULL, listenfd, -1, -1, worker_accept_conn);
    proxy->fdo.socket_in.read_write_skip = 1;
    fdo_set_state(&proxy->fdo, 0, FDH_TYPE_SOCKET_IN);

    return MYSW_OK;
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
    proxy->fdpoll->done = 1;
    /* TODO broadcast all conditions */

    return NULL;
}

static void signal_handle(int signum) {
    write(done_pipe[1], &signum, sizeof(int));
}

static int signal_block_all() {
    sigset_t set;
    sigfillset(&set);
    if (sigprocmask(SIG_BLOCK, &set, NULL) == 0) {
        perror("signal_block_all: sigprocmask");
        return MYSW_OK;
    }
    return MYSW_ERR;
}

static int main_deinit(proxy_t *proxy) {

    server_destroy(server_a);
    server_destroy(server_b);
    pool_destroy(pool_a);


    if (proxy->signal_thread) {
        signal_handle(0);
        pthread_join(*proxy->signal_thread, NULL);
    }
    if (proxy->fdo.socket_in.fd != -1) {
        close(proxy->fdo.socket_in.fd);
        fdo_deinit(&proxy->fdo);
    }
    if (proxy->fdpoll) fdpoll_free(proxy->fdpoll);
    if (proxy->workers) free(proxy->workers);
    /* TODO if (proxy->pool_map) pool_free_all(proxy); */
    /* if (proxy->targeter) targeter_free(proxy->targeter); */
    if (proxy->spinlock_pool_map) pthread_spin_destroy(proxy->spinlock_pool_map);
    free(proxy);
    return MYSW_OK;
}
