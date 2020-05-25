#include "mysw.h"

static void signal_handle(int signum);
static void signal_write_done();

void *signal_main(void *arg) {
    int rv, ignore;
    ssize_t iorv;
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
    iorv = read(mysw.done_pipe[0], &ignore, sizeof(int));
    (void)iorv;

    // set done flag
    mysw.done = 1;
    // TODO wakeup threads waiting on condition

    return NULL;
}

int signal_block_all() {
    int rv;
    sigset_t set;
    if_err_return(rv, sigfillset(&set));
    if_err_return(rv, sigprocmask(SIG_BLOCK, &set, NULL));
    return MYSW_OK;
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
