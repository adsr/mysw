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
#include <sys/epoll.h>
#include "aco.h"

#define if_err_return(rv, expr) if (((rv) = (expr)) != 0) return rv
#define if_err_break(rv, expr)  if (((rv) = (expr)) != 0) break
#define if_err(rv, expr)        if (((rv) = (expr)) != 0)

#define MYSW_OK  0
#define MYSW_ERR 1
#define MYSW_FDO_TYPE_CLIENT  1
#define MYSW_FDO_TYPE_BACKEND 2

typedef struct _mysw_t     mysw_t;
typedef struct _buf_t      buf_t;
typedef struct _acceptor_t acceptor_t;
typedef struct _worker_t   worker_t;
typedef struct _client_t   client_t;
typedef struct _backend_t  backend_t;
typedef struct _targeter_t targeter_t;
typedef struct _mthread_t  mthread_t;
typedef struct _fdo_t      fdo_t;

struct _mthread_t {
    pthread_t thread;
    int created;
};

struct _fdo_t {
    int type;
    union {
        client_t *client;
        backend_t *backend;
    } owner;
    struct epoll_event *last_event;
    aco_t *co;
    aco_share_stack_t *stack;
    int co_dead;
};

struct _mysw_t {
    int opt_num_workers;
    int opt_num_epoll_events;
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
    mthread_t thread;
    int epollfd;
    struct epoll_event *events;
    aco_t *main_co;
};

struct _client_t {
    int socketfd;
    int eventfd;
    int timerfd;
    fdo_t fdo;
};

struct _backend_t {
    int socketfd;
    int eventfd;
    int timerfd;
    fdo_t fdo;
    char *host;
    int port;
    char *dbname;
};

struct _targeter_t {
    int num;
    int eventfd;
    pthread_t thread;
    int thread_created;
};

void *signal_main(void *arg);
int signal_block_all();
void *worker_main(void *arg);
void client_handle();
void backend_handle();

extern mysw_t mysw;

#endif
