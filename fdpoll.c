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
    /* Previously I was using EPOLLONESHOT for thread safety on older kernels,
     * but I've had trouble getting that to work consistently. I am using the
     * new hotness, EPOLLEXCLUSIVE, for now. */
    fdpoll->epoll_flags = EPOLLEXCLUSIVE;
    fdpoll->udata = udata;
    *out_fdpoll = fdpoll;
    return MYSW_OK;
}

int fdpoll_free(fdpoll_t *fdpoll) {
    close(fdpoll->epoll_fd);
    free(fdpoll);
    return MYSW_OK;
}

int fdpoll_set_done(fdpoll_t *fdpoll) {
    fdpoll->done = 1;
    return MYSW_OK;
}

int fdpoll_event_loop(fdpoll_t *fdpoll) {
    int nfds, i;
    struct epoll_event *events;
    fdh_t *fdh;

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

            /* Re-arm before doing i/o */
            /* See https://stackoverflow.com/a/14241095/6048039 */
            fdh->state = FDH_STATE_RAISED;
            fdh_watch(fdh);

            /* Invoke read/write handler (usually fdh_read_write) */
            if (fdh->fn_read_write) {
                (fdh->fn_read_write)(fdh);
            }

            /* Invoke type-specific handler (client_process, server_process, etc) */
            if (fdh->fn_process) {
                (fdh->fn_process)(fdh);
            }
        }
    }

    free(events);
    return MYSW_OK;
}

int fdh_init(fdh_t *fdh, fdpoll_t *fdpoll, int type, void *udata, int fd, int (*fn_read_write)(fdh_t *), int (*fn_process)(fdh_t *)) {
    memset(fdh, 0, sizeof(fdh_t));
    fdh->fdpoll = fdpoll;
    fdh->type = type;
    fdh->udata = udata;
    fdh->fd = fd;
    fdh->fn_process = fn_process;
    fdh->fn_read_write = fn_read_write;
    fdh->state = FDH_STATE_UNWATCHED;
    return MYSW_OK;
}

int fdh_ensure_watched(fdh_t *fdh) {
    switch (fdh->state) {
        case FDH_STATE_UNWATCHED: return fdh_watch(fdh);
        case FDH_STATE_RAISED:    return fdh_watch(fdh);
        case FDH_STATE_WATCHED:   return MYSW_OK;
    }
    fprintf(stderr, "fdh_ensure_watched: Invalid state %d\n", fdh->state);
    return MYSW_ERR;
}

int fdh_ensure_unwatched(fdh_t *fdh) {
    switch (fdh->state) {
        case FDH_STATE_UNWATCHED: return MYSW_OK;
        case FDH_STATE_RAISED:    return fdh_unwatch(fdh);;
        case FDH_STATE_WATCHED:   return fdh_unwatch(fdh);
    }
    fprintf(stderr, "fdh_ensure_unwatched: Invalid state %d\n", fdh->state);
    return MYSW_ERR;
}

int fdh_watch(fdh_t *fdh) {
    int rv, epoll_op;
    struct epoll_event ev;
    uint32_t epoll_flags;

    if (fdh->state != FDH_STATE_UNWATCHED && fdh->state != FDH_STATE_RAISED) {
        fprintf(stderr, "fdh_watch: Expected FDH_STATE_UNWATCHED or FDH_STATE_RAISED\n");
        return MYSW_ERR;
    }

    /* Use edge-triggered behavior for reads. This means we need to read in a
     * loop until EAGAIN in fdh_read. For writes, we cannot always write until
     * EAGAIN, so use default level-triggered behavior there. */
    /* TODO is EPOLLET ok for evetnfd and timerfd? */
    if (fdh->fn_read_write == fdh_read_write && (fdh->epoll_flags & EPOLLIN)) {
        epoll_flags = EPOLLET;
    } else {
        epoll_flags = 0;
    }

    /* Init epoll_event */
    memset(&ev, 0, sizeof(ev));
    ev.events = fdh->fdpoll->epoll_flags | fdh->epoll_flags | epoll_flags;
    ev.data.ptr = fdh;

    if (fdh->state == FDH_STATE_RAISED) {
        /* TODO EPOLLONESHOT */
        /* Do nothing in EPOLLEXCLUSIVE mode */
    } else {
        epoll_op = (fdh->state == FDH_STATE_RAISED) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

        /* Add to epoll */
        if ((rv = epoll_ctl(fdh->fdpoll->epoll_fd, epoll_op, fdh->fd, &ev)) < 0) {
            perror("fdh_watch: epoll_ctl");
            return MYSW_ERR;
        }
    }

    fdh->state = FDH_STATE_WATCHED;
    return MYSW_OK;
}

int fdh_unwatch(fdh_t *fdh) {
    int rv;
    if (fdh->state != FDH_STATE_WATCHED && fdh->state != FDH_STATE_RAISED) {
        fprintf(stderr, "fdh_unwatch: Expected FDH_STATE_WATCHED or FDH_STATE_RAISED\n");
        return MYSW_ERR;
    }
    if ((rv = epoll_ctl(fdh->fdpoll->epoll_fd, EPOLL_CTL_DEL, fdh->fd, NULL)) < 0) {
        perror("fdh_unwatch: epoll_ctl");
        return MYSW_ERR;
    }
    fdh->state = FDH_STATE_UNWATCHED;
    return MYSW_OK;
}

int fdh_read_write(fdh_t *fdh) {
    int rv;
    if (fdh->epoll_flags & EPOLLIN) {
        rv = fdh_read(fdh);
    }
    if (fdh->epoll_flags & EPOLLOUT) {
        rv = fdh_write(fdh);
    }
    return rv;
}

int fdh_read_u64(fdh_t *fdh) {
    ssize_t rv;
    uint64_t i;

    /* TODO assert epoll_flags EPOLLIN */
    rv = read(fdh->fd, &i, sizeof(uint64_t));
    if (rv < 0) {
        /* Read error */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* Try again later */
            rv = 0;
        } else {
            /* Fatal error */
            fdh->read_write_errno = errno;
        }
    } else if (rv != sizeof(uint64_t)) {
        /* eventfd and timerfd should always come in uint64_t */
        rv = -1;
    }
    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_read(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *in;
    uchar *buf;

    in = &fdh->in;

    len = in->len;
    buf_ensure_cap(in, len + opt_read_size + 1);
    buf = in->data + len;

    /* Read until EAGAIN/EWOULDBLOCK, EOF, or error. Once we get
     * EAGAIN/EWOULDBLOCK. */
    while (1) {
        rv = read(fdh->fd, buf, opt_read_size);
        if (rv < 0) {
            /* Read error */
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* Try again later */
                rv = 0;
            } else {
                /* Fatal error */
                fdh->read_write_errno = errno;
            }
            break;
        } else if (rv == 0) {
            /* Read EOF */
            fdh->read_eof = 1;
            break;
        } else {
            /* Read success */
            in->len += rv;
            *(in->data + in->len) = '\0';
        }
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_write(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *out;
    uchar *buf;

    out = &fdh->out;

    len = out->len - fdh->out_cursor;
    buf = out->data + fdh->out_cursor;

    rv = write(fdh->fd, buf, len);
    if (rv == -1) {
        /* Write error */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* Try again later */
            rv = 0;
        } else {
            /* Fatal error */
            fdh->read_write_errno = errno;
        }
    } else if (rv < (ssize_t)len) {
        /* Partial write */
        fdh->out_cursor += rv;
    } else {
        /* Finished write */
        fdh->out_cursor = len;
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_is_write_finished(fdh_t *fdh) {
    if (fdh_is_writing(fdh) && fdh->out_cursor >= fdh->out.len) {
        return 1;
    }
    return 0;
}

int fdh_is_writing(fdh_t *fdh) {
    if (fdh->out.len > 0) {
        return 1;
    }
    return 0;
}

int fdh_set_read_write(fdh_t *fdh, int (*fn_read_write)(fdh_t *)) {
    fdh->fn_read_write = fn_read_write;
    return MYSW_OK;
}

int fdh_set_epoll_flags(fdh_t *fdh, int epoll_flags) {
    fdh->epoll_flags = epoll_flags;
    return MYSW_OK;
}

int fdh_reset_rw_state(fdh_t *fdh) {
    buf_clear(&fdh->in);
    buf_clear(&fdh->out);
    fdh->out_cursor = 0;
    fdh->read_write_errno = 0;
    fdh->read_eof = 0;
    return MYSW_OK;
}
