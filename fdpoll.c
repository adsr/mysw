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
            fdh->epoll_events = events[i].events;

            /* Lock */
            if (fdh->spinlock) {
                pthread_spin_lock(fdh->spinlock);
            }

            /* Another thread handled and unwatched us */
            if (fdh->state == FDH_STATE_UNWATCHED) {
                pthread_spin_unlock(fdh->spinlock);
                continue;
            }

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
            if (fdh->fn_process) {
                (fdh->fn_process)(fdh);
            }

            /* Unlock */
            if (fdh->spinlock) {
                pthread_spin_unlock(fdh->spinlock);
            }
        }
    }

    free(events);
    return MYSW_OK;
}

int fdh_init(fdh_t *r, fdh_t *w, fdpoll_t *fdpoll, void *udata, pthread_spinlock_t *spinlock, int type, int rfd, int (*fn_process)(fdh_t *)) {
    int wfd;

    memset(r, 0, sizeof(fdh_t));
    r->fdpoll = fdpoll;
    r->udata = udata;
    r->fn_process = fn_process;
    r->type = type;
    r->fd = rfd;
    r->is_writeable = 0;
    r->spinlock = spinlock;

    if (w) {
        wfd = dup(rfd);
        if (wfd < 0) {
            perror("fdh_init: dup");
            return MYSW_ERR;
        }
        memset(w, 0, sizeof(fdh_t));
        w->fdpoll = fdpoll;
        w->udata = udata;
        w->fn_process = fn_process;
        w->type = type;
        w->fd = wfd;
        w->is_writeable = 1;
        w->spinlock = spinlock;
    }

    return MYSW_OK;
}

int fdh_deinit(fdh_t *r, fdh_t *w) {
    buf_free(&r->buf);
    memset(r, 0, sizeof(fdh_t));
    if (w) {
        buf_free(&w->buf);
        close(w->fd);
        memset(w, 0, sizeof(fdh_t));
    }
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
    epoll_flags = fdh->is_writeable ? EPOLLOUT : EPOLLIN;

    /* Use edge-triggered behavior for socket reads */
    if (fdh->type == FDH_TYPE_SOCKET && !fdh->is_writeable) {
        epoll_flags |= EPOLLET;
    }

    /* Init epoll_event */
    memset(&ev, 0, sizeof(ev));
    ev.events = fdh->fdpoll->epoll_flags | epoll_flags;
    ev.data.ptr = fdh;

    /* Add to epoll */
    epoll_op = (fdh->state == FDH_STATE_ONESHOT) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if ((epoll_ctl(fdh->fdpoll->epoll_fd, epoll_op, fdh->fd, &ev)) < 0) {
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
    if ((epoll_ctl(fdh->fdpoll->epoll_fd, EPOLL_CTL_DEL, fdh->fd, NULL)) < 0) {
        perror("fdh_unwatch: epoll_ctl");
        return MYSW_ERR;
    }

    fdh->state = FDH_STATE_UNWATCHED;
    return MYSW_OK;
}

int fdh_read(fdh_t *fdh) {
    if (fdh->type == FDH_TYPE_EVENT) {
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
    if (fdh->is_writeable && fdh->buf.len > 0) {
        return 1;
    }
    return 0;
}

int fdh_reset_rw_state(fdh_t *fdh) {
    buf_clear(&fdh->buf);
    fdh->epoll_events = 0;
    fdh->buf_cursor = 0;
    fdh->read_write_errno = 0;
    fdh->read_eof = 0;
    return MYSW_OK;
}
