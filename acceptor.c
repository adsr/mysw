#include "mysw.h"

static void *acceptor_main(void *arg);

int acceptor_create_all() {
    int rv, i;
    acceptor_t *acceptor;

    // allocate acceptors
    mysw.acceptors = calloc(mysw.opt_num_acceptors, sizeof(acceptor_t));
    if (!mysw.acceptors) {
        perror("create_acceptors: calloc");
        return MYSW_ERR;
    }

    // create acceptor threads
    for (i = 0; i < mysw.opt_num_acceptors; ++i) {
        acceptor = mysw.acceptors + i;
        acceptor->num = i;
        if_err_return(rv, pthread_create(&acceptor->thread.thread, NULL, acceptor_main, acceptor));
        acceptor->thread.created = 1;
    }

    return MYSW_OK;
}

int acceptor_join_all() {
    int rv, i;
    acceptor_t *acceptor;

    // join acceptor threads
    for (i = 0; i < mysw.opt_num_acceptors; ++i) {
        acceptor = mysw.acceptors + i;
        if (acceptor->thread.created) {
            if_err_return(rv, pthread_join(acceptor->thread.thread, NULL));
        }
    }

    return MYSW_OK;
}

int acceptor_free_all() {
    // bail if not allocated
    if (!mysw.acceptors) {
        return MYSW_OK;
    }

    free(mysw.acceptors);

    return MYSW_OK;
}

static void *acceptor_main(void *arg) {
    int rv, socketfd;
    fd_set rfds;
    struct timeval timeout;
    struct epoll_event event;
    client_t *client;
    uint64_t u64;

    (void)arg;
    u64 = 1;

    // loop until done
    while (!mysw.done) {
        FD_ZERO(&rfds);
        FD_SET(mysw.listenfd, &rfds);
        timeout.tv_sec = mysw.opt_acceptor_select_timeout_s;
        timeout.tv_usec = 0;

        // select on listenfd
        rv = select(mysw.listenfd + 1, &rfds, NULL, NULL, &timeout);
        if (rv < 0) {
            // select error
            perror("acceptor_main: select");
            break;
        } else if (rv == 0) {
            // no activity
            continue;
        }

        // accept client conn
        if ((socketfd = accept4(mysw.listenfd, NULL, NULL, SOCK_NONBLOCK)) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // another acceptor got it first
                continue;
            } else {
                // accept error
                perror("acceptor_main: accept");
                break;
            }
        }

        // find unused client
        pthread_spin_lock(&mysw.clients_lock.spinlock);
        client = mysw.clients_unused;
        if (client) {
            mysw.clients_unused = client->next_unused;
        }
        pthread_spin_unlock(&mysw.clients_lock.spinlock);

        // reject if no more slots
        if (!client) {
            // TODO send err packet
            close(socketfd);
            continue;
        }

        // write to client eventfd
        if (write(client->fdh_event.fd, &u64, sizeof(u64)) != sizeof(u64)) {
            perror("acceptor_main: write");
            // TODO reset client, send err packet
            continue;
        }

        // add client to worker
        client->fdh_socket.fd = socketfd;
        event.events = EPOLLIN;
        event.data.ptr = &client->fdh_socket;
        if (epoll_ctl(client->worker->epollfd, EPOLL_CTL_ADD, client->fdh_socket.fd, &event) < 0) {
            perror("acceptor_main: epoll_ctl");
            break;
        }
    }

    return NULL;
}
