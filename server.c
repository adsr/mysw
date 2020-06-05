#include "mysw.h"

// grep ^static server.c | sed 's| {|;|g'
static void server_handle();
static int server_deinit(server_t *server);
static int server_handle_connect(server_t *server);
static int server_set_wakeup_timer_and_yield(server_t *server, long millis);
static int server_handle_handshake_read(server_t *server);
static int server_handle_handshake_write_and_response(server_t *server);
static int server_handle_command(server_t *server);

static void server_handle() {
    int rv;
    uint64_t u64;
    server_t *server;

    server = (server_t*)aco_get_arg();

    // drain event
    read(server->fdh_event.fd, &u64, sizeof(u64));

    // server loop
    while (!mysw.done) {
        if_err_goto(rv, server_handle_connect(server), server_handle_exit);
        if_err_goto(rv, server_handle_handshake_read(server), server_handle_exit);
        if_err_goto(rv, server_handle_handshake_write_and_response(server), server_handle_exit);
        while (!mysw.done && server->alive) {
            if_err_break(rv, server_handle_command(server));
        }

server_handle_exit:
        server_deinit(server);

        if_err_break(rv, server_set_wakeup_timer_and_yield(server, 1000)); // TODO opt
    }

    aco_exit();
}

static int server_deinit(server_t *server) {
    struct itimerspec its;
    uint64_t u64;

    // TODO release target client if needed
    if (server->target_client) {
    }

    // close socket if needed
    if (server->fdh_socket.fd >= 0) {
        epoll_ctl(server->worker->epollfd, EPOLL_CTL_DEL, server->fdh_socket.fd, NULL);
        close(server->fdh_socket.fd);
        server->fdh_socket.fd = -1;
    }

    // disarm timerfd
    memset(&its, 0, sizeof(its));
    timerfd_settime(server->fdh_timer.fd, 0, &its, NULL);

    // drain eventfd (should return EAGAIN if no events)
    read(server->fdh_event.fd, &u64, sizeof(u64));

    // reset buffers
    buf_clear(&server->wbuf);
    buf_clear(&server->rbuf);
}

static int server_handle_connect(server_t *server) {
    int sockfd, sock_flags;
    int rv;
    struct sockaddr_in addr;
    socklen_t sock_error_len;
    int sock_error;
    struct epoll_event event;

    // connect loop
    do {
        // make non-blocking socket
        if ((server->fdh_socket.fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0) {
            perror("server_handle_connect: socket");
            goto server_handle_connect_try_again;
        }

        // connect
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(server->ip4);
        addr.sin_port = htons(server->port);
        rv = connect(server->fdh_socket.fd, (struct sockaddr *)&addr, sizeof(addr));

        if (rv < 0) {
            if (errno != EINPROGRESS) {
                // connect error
                perror("server_handle_connect: connect");
                goto server_handle_connect_try_again;
            } else {
                // connect in progress

                // poll for completion
                // add socket to epoll with EPOLLOUT | EPOLLONESHOT
                event.events = EPOLLOUT | EPOLLONESHOT;
                event.data.ptr = &server->fdh_socket;
                rv = epoll_add_or_mod(server->worker->epollfd, server->fdh_socket.fd, &event);
                if (rv != 0) {
                    perror("server_handle_connect: connect epoll_add_or_mod");
                    goto server_handle_connect_try_again;
                }
                aco_yield();

                // check for connection error
                sock_error_len = sizeof(sock_error);
                if (getsockopt(server->fdh_socket.fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) != 0) {
                    perror("server_handle_connect: getsockopt");
                    goto server_handle_connect_try_again;
                } else if (sock_error != 0) {
                    fprintf(stderr, "server_handle_connect: async connect: %s\n", strerror(sock_error));
                    goto server_handle_connect_try_again;
                }
            }
        }

        // connected!
        // add server to worker
        event.events = EPOLLIN;
        event.data.ptr = &server->fdh_socket;
        if (epoll_add_or_mod(server->worker->epollfd, server->fdh_socket.fd, &event) < 0) {
            perror("server_handle_connect: worker epoll_add_or_mod");
            goto server_handle_connect_try_again;
        }
        return MYSW_OK;

server_handle_connect_try_again:
        // close socket if open
        if (server->fdh_socket.fd >= 0) {
            close(server->fdh_socket.fd);
            server->fdh_socket.fd = -1;
        }

        // try again later
        if_err_return(rv, server_set_wakeup_timer_and_yield(server, 1000)); // TODO opt
    } while (!mysw.done);

    return MYSW_OK;
}

static int server_set_wakeup_timer_and_yield(server_t *server, long millis) {
    int rv;
    uint64_t u64;
    struct itimerspec its;
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_sec = millis / 1000;
    its.it_value.tv_nsec = (millis % 1000) * 1000000;
    if (timerfd_settime(server->fdh_timer.fd, CLOCK_MONOTONIC, &its, NULL) != 0) {
        perror("server_set_wakeup_timer_and_yield: timerfd_settime");
        return MYSW_ERR;
    }

    do {
        // TODO we could wakeup on a socket or eventfd event here
        // TODO use a state machine instead of doing this
        aco_yield();
    } while (read(server->fdh_timer.fd, &u64, sizeof(u64)) != sizeof(u64) || u64 != 1);

    return MYSW_OK;
}

static int server_handle_handshake_read(server_t *server) {
    return MYSW_ERR;
}

static int server_handle_handshake_write_and_response(server_t *server) {
    return MYSW_ERR;
}

static int server_handle_command(server_t *server) {
    return MYSW_ERR;
}

int server_create_all() {
    int rv;
    pool_t *pool, *pool_tmp;
    server_t *server;
    int i, server_num;
    struct epoll_event event;
    uint64_t u64;

    HASH_ITER(hh, mysw.pool_map, pool, pool_tmp) {
        // allocate servers
        pool->servers = calloc(pool->num_servers, sizeof(server_t));
        pool->servers_unused = pool->servers;
        if ((rv = pthread_spin_init(&pool->lock.spinlock, PTHREAD_PROCESS_SHARED)) != 0) {
            errno = rv;
            perror("server_create_all: pthread_spin_init");
            return MYSW_ERR;
        }
        pool->lock.created = 1;

        // init fds to -1
        for (i = 0; i < pool->num_servers; ++i) {
            server = pool->servers + i;
            server->fdh_socket.fd = -1;
            server->fdh_event.fd = -1;
            server->fdh_timer.fd = -1;
        }

        // init each server
        for (i = 0; i < pool->num_servers; ++i) {
            server = pool->servers + i;
            server->num = server_num;
            server_num += 1;

            // copy pool template
            server->host = pool->server_tpl.host;
            server->port = pool->server_tpl.port;
            server->ip4 = pool->server_tpl.ip4;
            server->db_name = pool->server_tpl.db_name;

            // init next_unused
            if (i + 1 < pool->num_servers) {
                server->next_unused = pool->servers + i + 1;
            }

            // init fdo
            server->fdo.co_func = server_handle;
            server->fdo.co_arg = server;
            server->fdo.alive = &server->alive;
            server->fdh_socket.fdo = &server->fdo;
            server->fdh_event.fdo = &server->fdo;
            server->fdh_timer.fdo = &server->fdo;

            // assign worker
            server->worker = mysw.workers + (server->num % mysw.opt_num_workers);

            // create eventfd
            if ((server->fdh_event.fd = eventfd(0, EFD_NONBLOCK)) < 0) {
                perror("server_create_all: eventfd");
                return MYSW_ERR;
            }

            // create timerfd
            if ((server->fdh_timer.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
                perror("server_create_all: timerfd_create");
                return MYSW_ERR;
            }

            // add eventfd to worker epoll
            event.events = EPOLLIN;
            event.data.ptr = &server->fdh_event;
            if (epoll_ctl(server->worker->epollfd, EPOLL_CTL_ADD, server->fdh_event.fd, &event) < 0) {
                perror("server_create_all: epoll_ctl");
                return MYSW_ERR;
            }

            // add timertfd to worker epoll
            event.events = EPOLLIN;
            event.data.ptr = &server->fdh_timer;
            if (epoll_ctl(server->worker->epollfd, EPOLL_CTL_ADD, server->fdh_timer.fd, &event) < 0) {
                perror("server_create_all: epoll_ctl");
                return MYSW_ERR;
            }

            // mark alive
            server->alive = 1;

            // write to eventfd to trigger worker
            u64 = 1;
            if (write(server->fdh_event.fd, &u64, sizeof(u64)) != sizeof(u64)) {
                fprintf(stderr, "server_create_all: Failed to write to eventfd\n");
                return MYSW_ERR;
            }

        } // foreach server
    } // foreach pool

    return MYSW_OK;
}

int server_free_all() {
    pool_t *pool, *pool_tmp;
    server_t *server;
    int i, server_num;
    fdo_t *fdo;

    HASH_ITER(hh, mysw.pool_map, pool, pool_tmp) {

        // bail if not allocated
        if (!pool->servers) {
            continue;
        }

        // free lock
        if (pool->lock.created) {
            pthread_spin_destroy(&pool->lock.spinlock);
        }

        // free each server
        for (i = 0; i < pool->num_servers; ++i) {
            server = pool->servers + i;

            // deinit (closes socket)
            server_deinit(server);

            // close eventfd
            if (server->fdh_event.fd >= 0) {
                epoll_ctl(server->worker->epollfd, EPOLL_CTL_DEL, server->fdh_event.fd, NULL);
                close(server->fdh_event.fd);
            }

            // close timerfd
            if (server->fdh_timer.fd >= 0) {
                epoll_ctl(server->worker->epollfd, EPOLL_CTL_DEL, server->fdh_timer.fd, NULL);
                close(server->fdh_timer.fd);
            }

            // free coroutine resources
            fdo = &server->fdo;
            if (fdo->co) aco_destroy(fdo->co);
            if (fdo->co_stack) aco_share_stack_destroy(fdo->co_stack);
        }

        // free pool
        free(pool->servers);
        free(pool->name);
        free(pool);
    }

    return MYSW_OK;
}

int server_wakeup(server_t *server) {
    return MYSW_ERR;
}
