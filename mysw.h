#ifndef _MYSW_H
#define _MYSW_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include "aco.h"

#define if_err_return(rv, expr) if (((rv) = (expr)) != 0) return rv
#define if_err_break(rv, expr)  if (((rv) = (expr)) != 0) break
#define if_err(rv, expr)        if (((rv) = (expr)) != 0)

#define MYSW_OK  0
#define MYSW_ERR 1
#define MYSW_FDO_TYPE_CLIENT  1
#define MYSW_FDO_TYPE_BACKEND 2

typedef struct _acceptor_t acceptor_t;
typedef struct _backend_t  backend_t;
typedef struct _buf_t      buf_t;
typedef struct _client_t   client_t;
typedef struct _fdo_t      fdo_t;
typedef struct _fdh_t      fdh_t;
typedef struct _mlock_t    mlock_t;
typedef struct _mthread_t  mthread_t;
typedef struct _mysw_t     mysw_t;
typedef struct _targeter_t targeter_t;
typedef struct _worker_t   worker_t;

struct _mthread_t {
    pthread_t thread;
    int created;
};

struct _mlock_t {
    pthread_spinlock_t spinlock;
    int created;
};

struct _fdo_t {
    int type;
    union {
        client_t *client;
        backend_t *backend;
    } owner;
    struct epoll_event *event;
    aco_t *co;
    aco_share_stack_t *stack;
    int co_dead;
};

struct _fdh_t {
    fdo_t *fdo;
    int fd;
};

struct _mysw_t {
    char *opt_addr;
    int opt_port;
    int opt_backlog;
    int opt_num_workers;
    int opt_num_acceptors;
    int opt_num_epoll_events;
    int opt_max_num_clients;
    int opt_worker_epoll_timeout_ms;
    int opt_acceptor_select_timeout_s;
    worker_t *workers;
    acceptor_t *acceptors;
    backend_t *backends;
    targeter_t *targeters;
    mthread_t signal_thread;

    client_t *clients;
    client_t *clients_unused;
    mlock_t clients_lock;

    int listenfd;
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
    mthread_t thread;
};

struct _worker_t {
    int num;
    mthread_t thread;
    int epollfd;
    struct epoll_event *events;
    aco_t *main_co;
};

struct _client_t {
    int num;
    worker_t *worker;
    fdo_t fdo;
    fdh_t fdh_socket;
    fdh_t fdh_event;
    fdh_t fdh_timer;
    client_t *next_unused;
};

struct _backend_t {
    char *host;
    int port;
    char *dbname;
    fdo_t fdo;
    fdh_t fdh_socket;
    fdh_t fdh_event;
    fdh_t fdh_timer;
};

struct _targeter_t {
    int num;
    fdh_t eventfdh;
    pthread_t thread;
    int thread_created;
};

void *signal_main(void *arg);
int signal_block_all();
void *worker_main(void *arg);
void *acceptor_main(void *arg);
void client_handle();
void backend_handle();

extern mysw_t mysw;

#endif
