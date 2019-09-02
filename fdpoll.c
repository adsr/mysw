#include "mysw.h"


int fdpoll_new(void *udata, fdpoll_t **out_fdpoll) {
    fdpoll_t *fdpoll;
    int epfd;

    if ((epfd = epoll_create(1)) < 0) {
        perror("epoll_create");
        return MYSW_ERR;
    }

    fdpoll = calloc(1, sizeof(fdpoll_t));
    fdpoll->epoll_fd = epfd;
    fdpoll->epoll_flags = EPOLLONESHOT; /* or EPOLLEXCLUSIVE on newer kernels */
    fdpoll->udata = udata;

    *out_fdpoll = fdpoll;

    return MYSW_OK;
}

int fdpoll_free(fdpoll_t *fdpoll) {
    close(fdpoll->epoll_fd);
    free(fdpoll);
    return MYSW_OK;
}

int fdpoll_event_loop(fdpoll_t *fdpoll) {
    int nfds, i;
    struct epoll_event *events;
    fdh_t *fdh;
    fdo_t *fdo;

    events = calloc(opt_epoll_max_events, sizeof(struct epoll_event));

    while (!fdpoll->done) {
        /* Wait for events */
        nfds = epoll_wait(fdpoll->epoll_fd, events, opt_epoll_max_events, opt_epoll_timeout_ms);
        if (nfds < 0) {
            if (errno != EINTR) perror("worker_main: epoll_wait");
            continue;
        }

        /* Handle events */
        for (i = 0; i < nfds; ++i) {
            fdh = (fdh_t *)events[i].data.ptr;
            fdo = fdh->fdo;
            fdh->epoll_events = events[i].events;

            /* Lock */
            if (fdo->uspinlock) pthread_spin_lock(fdo->uspinlock);

            /* TODO not sure if we need this
            |* Another thread handled and unwatched us *|
            if (fdh->state == FDH_STATE_UNWATCHED) {
                pthread_spin_unlock(fdo->uspinlock);
                continue;
            } */

            /* Re-arm before doing i/o in EPOLLONESHOT */
            /* See https://stackoverflow.com/a/14241095/6048039 */
            if (fdpoll->epoll_flags & EPOLLONESHOT) {
                fdh->state = FDH_STATE_ONESHOT;
                fdh_ensure_watched(fdh);
            }

            /* TODO EPOLLRDHUP EPOLLHUP EPOLLERR ? */

            if (!fdh->read_write_skip) {
                /* Handle reading */
                if (fdh->epoll_events & EPOLLIN) {
                    fdh_read(fdh);
                }

                /* Handle writing */
                if (fdh->epoll_events & EPOLLOUT) {
                    if (fdh_is_write_unfinished(fdh)) {
                        fdh_write(fdh);
                        if (fdh_is_write_finished(fdh)) {
                            fdh_ensure_unwatched(fdh);
                        }
                    } else {
                        fdh_ensure_unwatched(fdh);
                    }
                }
            }

            /* Invoke type-specific handler (client_process, server_process, etc) */
            (fdo->uprocess)(fdh);

            /* Unlock */
            if (fdo->uspinlock) pthread_spin_unlock(fdo->uspinlock);
        }
    }

    free(events);
    return MYSW_OK;
}

int fdo_init(fdo_t *fdo, fdpoll_t *fdpoll, void *udata, int *ustate, pthread_spinlock_t *uspinlock, int sockfd, int eventfd, int timerfd, int (*uprocess)(fdh_t *)) {
    memset(fdo, 0, sizeof(fdo_t));
    fdo->fdpoll = fdpoll;
    fdo->udata = udata;
    fdo->ustate = ustate;
    fdo->uspinlock = uspinlock;
    fdo->uprocess = uprocess;

    if (sockfd != -1) {
        fdo_init_socket(fdo, sockfd);
    }

    if (eventfd != -1) {
        fdh_init(&fdo->event_in, fdo, FDH_TYPE_EVENT_IN, eventfd);
    }

    if (timerfd != -1) {
        fdh_init(&fdo->timer_in, fdo, FDH_TYPE_TIMER_IN, timerfd);
    }

    return MYSW_OK;
}

int fdo_init_socket(fdo_t *fdo, int sockfd) {
    int wsockfd;
    wsockfd = dup(sockfd);
    if (wsockfd < 0) {
        perror("fdo_init_socket: dup");
        return MYSW_ERR;
    }
    fdh_init(&fdo->socket_in, fdo, FDH_TYPE_SOCKET_IN, sockfd);
    fdh_init(&fdo->socket_out, fdo, FDH_TYPE_SOCKET_OUT, wsockfd);
    return MYSW_OK;
}

int fdo_deinit(fdo_t *fdo) {
    fdo_set_state(fdo, 0, 0);
    fdh_deinit(&fdo->socket_in);
    close(fdo->socket_out.fd);
    fdh_deinit(&fdo->socket_out);
    fdh_deinit(&fdo->event_in); /* TODO drain (read until EAGAIN)? */
    fdh_deinit(&fdo->timer_in); /* TODO timerfd_settime disarm? */
    return MYSW_OK;
}

int fdo_set_state(fdo_t *fdo, int state, int watch_types) {
    int rv;
    if (!(watch_types & FDH_TYPE_SOCKET_IN))  try(rv, fdh_ensure_unwatched(&fdo->socket_in));
    if (!(watch_types & FDH_TYPE_SOCKET_OUT)) try(rv, fdh_ensure_unwatched(&fdo->socket_out));
    if (!(watch_types & FDH_TYPE_EVENT_IN))   try(rv, fdh_ensure_unwatched(&fdo->event_in));
    if (!(watch_types & FDH_TYPE_TIMER_IN))   try(rv, fdh_ensure_unwatched(&fdo->timer_in));
    if ( (watch_types & FDH_TYPE_SOCKET_IN))  try(rv, fdh_ensure_watched(&fdo->socket_in));
    if ( (watch_types & FDH_TYPE_SOCKET_OUT)) try(rv, fdh_ensure_watched(&fdo->socket_out));
    if ( (watch_types & FDH_TYPE_EVENT_IN))   try(rv, fdh_ensure_watched(&fdo->event_in));
    if ( (watch_types & FDH_TYPE_TIMER_IN))   try(rv, fdh_ensure_watched(&fdo->timer_in));
    if (fdo->ustate) *(fdo->ustate) = state;
    return MYSW_OK;
}

int fdh_init(fdh_t *fdh, fdo_t *fdo, int type, int fd) {
    memset(fdh, 0, sizeof(fdh_t));
    fdh->fdo = fdo;
    fdh->type = type;
    fdh->fd = fd;
    return MYSW_OK;
}

int fdh_deinit(fdh_t *fdh) {
    buf_free(&fdh->buf);
    return MYSW_OK;
}

int fdh_ensure_watched(fdh_t *fdh) {
    switch (fdh->state) {
        case FDH_STATE_WATCHED:   return MYSW_OK;
        case FDH_STATE_ONESHOT:   return fdh_watch(fdh);
        case FDH_STATE_UNWATCHED: return fdh_watch(fdh);
    }
    return MYSW_ERR;
}

int fdh_ensure_unwatched(fdh_t *fdh) {
    switch (fdh->state) {
        case FDH_STATE_WATCHED:   return fdh_unwatch(fdh);
        case FDH_STATE_ONESHOT:   return fdh_unwatch(fdh);
        case FDH_STATE_UNWATCHED: return MYSW_OK;
    }
    return MYSW_ERR;
}

int fdh_watch(fdh_t *fdh) {
    int epoll_op;
    struct epoll_event ev;
    uint32_t epoll_flags;

    /* Nothing to do if already watched */
    if (fdh->state == FDH_STATE_WATCHED) {
        fprintf(stderr, "fdh_watch: Already watched\n");
        return MYSW_OK;
    }

    /* Set appropriate flags */
    epoll_flags = fdh_is_writeable(fdh) ? EPOLLOUT : EPOLLIN;

    /* Use edge-triggered behavior for socket reads */
    if (fdh->type == FDH_TYPE_SOCKET_IN) {
        epoll_flags |= EPOLLET;
    }

    /* Init epoll_event */
    memset(&ev, 0, sizeof(ev));
    ev.events = fdh->fdo->fdpoll->epoll_flags | epoll_flags;
    ev.data.ptr = fdh;

    /* Add to epoll */
    epoll_op = (fdh->state == FDH_STATE_ONESHOT) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if ((epoll_ctl(fdh->fdo->fdpoll->epoll_fd, epoll_op, fdh->fd, &ev)) < 0) {
        perror("fdh_watch: epoll_ctl");
        return MYSW_ERR;
    }

    fdh->state = FDH_STATE_WATCHED;
    return MYSW_OK;
}

int fdh_unwatch(fdh_t *fdh) {
    /* Nothing to do if already unwatched */
    if (fdh->state == FDH_STATE_UNWATCHED) {
        fprintf(stderr, "fdh_watch: Already unwatched\n");
        return MYSW_OK;
    }

    /* Remove from epoll */
    if ((epoll_ctl(fdh->fdo->fdpoll->epoll_fd, EPOLL_CTL_DEL, fdh->fd, NULL)) < 0) {
        perror("fdh_unwatch: epoll_ctl");
        return MYSW_ERR;
    }

    fdh->state = FDH_STATE_UNWATCHED;
    return MYSW_OK;
}

int fdh_read(fdh_t *fdh) {
    if (fdh->type == FDH_TYPE_EVENT_IN) {
        return fdh_read_event(fdh);
    }
    return fdh_read_socket(fdh);
}

int fdh_read_event(fdh_t *fdh) {
    ssize_t rv;
    uint64_t i;

    /* Events (eventfd and timerfd) always come in uint64_t. (See
     * eventfd_create(2) and timerfd_create(2).) So we are going to try reading
     * one uint64_t here. */

    /* Read once (see `break` at end of loop), or while EINTR */
    while (1) {
        rv = read(fdh->fd, &i, sizeof(uint64_t));
        if (rv < 0) {
            /* Read error */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* The kernel lied to us? */
                rv = 0;
            } else if (errno == EINTR) {
                /* Try again */
                continue;
            }
            /* Fatal error */
            fdh->read_write_errno = errno;
        } else if (rv != sizeof(uint64_t)) {
            /* Hm, eventfd and timerfd should always come in uint64_t */
            rv = -1;
        }
        break;
    }
    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_read_socket(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *in;
    uchar *buf;

    in = &fdh->buf;

    len = in->len;
    buf_ensure_cap(in, len + opt_read_size + 1);
    buf = in->data + len;

    /* Read until EAGAIN/EWOULDBLOCK, EOF, or error */
    while (1) {
        rv = read(fdh->fd, buf, opt_read_size);
        if (rv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Read as much as we could */
                rv = 0;
                break;
            } else if (errno == EINTR) {
                /* Try again */
                continue;
            }
            /* Fatal error */
            fdh->read_write_errno = errno;
            break;
        } else if (rv == 0) {
            /* Read EOF */
            fdh->read_eof = 1;
            break;
        } else {
            /* Read success */
            in->len += rv;
            *(in->data + in->len) = '\0'; /* Be nice */
        }
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_write(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *out;
    uchar *buf;

    out = &fdh->buf;

    len = out->len - fdh->buf_cursor;
    buf = out->data + fdh->buf_cursor;

    while (1) {
        rv = write(fdh->fd, buf, len);
        if (rv == -1) {
            /* Write error */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Try again later */
                rv = 0;
            } else if (errno == EINTR) {
                continue;
            } else {
                /* Fatal error */
                fdh->read_write_errno = errno;
            }
        } else if (rv < (ssize_t)len) {
            /* Partial write */
            fdh->buf_cursor += rv;
        } else {
            /* Finished write */
            fdh->buf_cursor = len;
        }
        break;
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_is_write_finished(fdh_t *fdh) {
    if (fdh_is_writing(fdh) && fdh->buf_cursor >= fdh->buf.len) {
        return 1;
    }
    return 0;
}

int fdh_is_write_unfinished(fdh_t *fdh) {
    if (fdh_is_writing(fdh) && fdh->buf_cursor < fdh->buf.len) {
        return 1;
    }
    return 0;
}

int fdh_is_writing(fdh_t *fdh) {
    if (fdh_is_writeable(fdh) && fdh->buf.len > 0) {
        return 1;
    }
    return 0;
}

int fdh_is_writeable(fdh_t *fdh) {
    if (fdh->type == FDH_TYPE_SOCKET_OUT) {
        return 1;
    }
    return 0;
}

int fdh_clear(fdh_t *fdh) {
    buf_clear(&fdh->buf);
    fdh->epoll_events = 0;
    fdh->buf_cursor = 0;
    fdh->read_write_errno = 0;
    fdh->read_eof = 0;
    return MYSW_OK;
}
