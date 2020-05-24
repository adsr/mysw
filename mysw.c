#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "aco.h"

#define if_err_return(rv, expr) if (((rv) = (expr)) != 0) return rv
#define if_err_break(rv, expr)  if (((rv) = (expr)) != 0) break
#define if_err(rv, expr)        if (((rv) = (expr)) != 0)

typedef struct _mysw_t mysw_t;
typedef struct _buf_t buf_t;
typedef struct _acceptor_t acceptor_t;
typedef struct _worker_t worker_t;
typedef struct _client_t client_t;
typedef struct _backend_t backend_t;
typedef struct _targeter_t targeter_t;
typedef struct _mthread_t mthread_t;

struct _mthread_t {
    pthread_t thread;
    int created;
};

struct _mysw_t {
    worker_t *workers;
    acceptor_t *acceptors;
    client_t *clients;
    backend_t *backends;
    targeter_t *targeters;
    mthread_t signal_thread;
    int done_pipe[2];
    int done;
};

struct _buf_t {
    uint8_t *data;
    size_t len;
    size_t cap;
};

struct _acceptor_t {
    int num;
    pthread_t thread;
    int thread_created;
};

struct _worker_t {
    int num;
    pthread_t thread;
    int thread_created;
    int epollfd;
    aco_t *main_co;
};

struct _client_t {
    int socketfd;
    int eventfd;
    int timerfd;
    aco_t *co;
    aco_share_stack_t *stack;
};

struct _backend_t {
    int socketfd;
    int eventfd;
    int timerfd;
    char *host;
    int port;
    char *dbname;
    aco_t *co;
    aco_share_stack_t *stack;
};

struct _targeter_t {
    int num;
    int eventfd;
    pthread_t thread;
    int thread_created;
};

static int create_signal_thread();
static void signal_handle(int signum);
static void signal_write_done();
static void *signal_main(void *arg);
static int signal_block_all();

mysw_t mysw;

int main(int argc, char **argv) {
    int rv;

    (void)argc;
    (void)argv;

    memset(&mysw, 0, sizeof(mysw));
    mysw.done_pipe[0] = -1;
    mysw.done_pipe[1] = -1;
    rv = 0;

    do {
        if_err_break(rv, create_signal_thread());
        // if_err_break(rv, create_workers());
        // if_err_break(rv, create_targeters());
        // if_err_break(rv, create_clients());
        // if_err_break(rv, create_backends());
        // if_err_break(rv, create_listener());
        // if_err_break(rv, create_acceptors());
    } while (0);

    // rv |= join_acceptor_threads();
    // rv |= join_targeter_threads();
    // rv |= join_worker_threads();
    // rv |= join_signal_thread();

    // free_acceptors();
    // free_listener();
    // free_backends();
    // free_clients();
    // free_targeters();
    // free_workers();

    return rv;
}

static int create_signal_thread() {
    int rv;
    if_err_return(
        rv,
        pthread_create(&mysw.signal_thread.thread, NULL, signal_main, NULL)
    );
    mysw.signal_thread.created = 1;
    if_err_return(rv, signal_block_all());
    return 0;
}

static void signal_handle(int signum) {
    (void)signum;
    signal_write_done();
}

static void signal_write_done() {
    ssize_t rv;
    int any;
    if (mysw.done_pipe[1] >= 0) {
        any = 1;
        rv = write(mysw.done_pipe[1], &any, sizeof(any));
    }
    (void)rv;
}

static void *signal_main(void *arg) {
    int rv, ignore;
    ssize_t readrv;
    fd_set rfds;
    struct timeval tv;
    struct sigaction sa;

    (void)arg;

    // create done_pipe
    if_err(rv, pipe(mysw.done_pipe)) {
        perror("pipe");
        mysw.done = 1;
        return NULL;
    }
    if_err(rv, fcntl(mysw.done_pipe[1], F_SETFL, O_NONBLOCK)) {
        perror("fcntl");
        mysw.done = 1;
        return NULL;
    }

    // install signal handler
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_handler = signal_handle;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    // wait for write on done_pipe from signal_write_done
    do {
        FD_ZERO(&rfds);
        FD_SET(mysw.done_pipe[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        rv = select(mysw.done_pipe[0] + 1, &rfds, NULL, NULL, &tv);
    } while (rv < 1);

    // read pipe for fun
    readrv = read(mysw.done_pipe[0], &ignore, sizeof(int));
    (void)readrv;

    // set done flag
    mysw.done = 1;
    // TODO wakeup threads waiting on condition

    return NULL;
}

static int signal_block_all() {
    int rv;
    sigset_t set;
    if_err_return(rv, sigfillset(&set));
    if_err_return(rv, sigprocmask(SIG_BLOCK, &set, NULL));
    return 0;
}
