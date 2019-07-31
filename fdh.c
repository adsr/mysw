#include "mysw.h"

static int fdh_watch_inner(fdh_t *fdh, int rewatch) {
    int rv, epoll_op;
    struct epoll_event ev;

    /* Init epoll_event */
    memset(&ev, 0, sizeof(ev));
    #if 0 && defined(EPOLLEXCLUSIVE)
        ev.events = EPOLLET | EPOLLEXCLUSIVE | fdh->epoll_flags;
    #else
        ev.events = EPOLLET | EPOLLONESHOT | fdh->epoll_flags;
    #endif
    ev.data.ptr = fdh;

    epoll_op = rewatch ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

    /* Add to epoll */
    if ((rv = epoll_ctl(fdh->proxy->epfd, epoll_op, fdh->fd, &ev)) < 0) {
        perror("fdh_watch_inner: epoll_ctl");
        return MYSW_ERR;
    }

    return MYSW_OK;
}

int fdh_watch(fdh_t *fdh) {
    return fdh_watch_inner(fdh, 0);
}

int fdh_rewatch(fdh_t *fdh) {
    #if 0 && defined(EPOLLEXCLUSIVE)
        (void)fdh;
        return MYSW_OK;
    #else
        return fdh_watch_inner(fdh, 1);
    #endif
}

int fdh_unwatch(fdh_t *fdh) {
    #if 0 && defined(EPOLLEXCLUSIVE)
        struct epoll_event ev;
        if (epoll_ctl(fdh->proxy->epfd, EPOLL_CTL_DEL, fdh->fd, &ev) < 0) {
            perror("fdh_unwatch: epoll_ctl");
            return MYSW_ERR;
        }
        return MYSW_OK;
    #else
        (void)fdh; /* TODO EPOLL_CTL_DEL needed? */
        return MYSW_OK;
    #endif
}

int fdh_read_write(fdh_t *fdh) {
    int rv;
    if (fdh->epoll_flags & EPOLLIN) { /* TODO assumed not both EPOLL(IN|OUT) */
        rv = fdh_read(fdh);
    } else if (fdh->epoll_flags & EPOLLOUT) {
        rv = fdh_write(fdh);
    }
    return rv;
}

int fdh_read(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *in;
    char *buf;

    in = &fdh->in;

    len = in->len;
    buf_ensure_cap(in, len + opt_read_size + 1);
    buf = in->data + len;

    rv = read(fdh->fd, buf, opt_read_size);
    if (rv == -1) {
        /* Read error */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* Try again later */
            rv = 0;
        } else {
            /* Fatal error */
            fdh->read_write_errno = errno;
        }
    } else if (rv > 0) {
        /* Read success */
        in->len += rv;
        *(in->data + in->len) = '\0';
    } else {
        /* Read EOF */
        fdh->read_eof = 1;
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_write(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *out;
    char *buf;

    out = &fdh->out;

    len = out->len - fdh->write_pos;
    buf = out->data + fdh->write_pos;

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
        fdh->write_pos += rv;
    } else {
        /* Finished write */
        fdh->write_pos = len;
    }

    return rv < 0 ? MYSW_ERR : MYSW_OK;
}

int fdh_is_write_finished(fdh_t *fdh) {
    if (fdh_is_writing(fdh) && fdh->write_pos >= fdh->out.len) {
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

int fdh_reset_rw_state(fdh_t *fdh) {
    buf_clear(&fdh->in);
    buf_clear(&fdh->out);
    fdh->write_pos = 0;
    fdh->read_write_errno = 0;
    fdh->read_eof = 0;
    return MYSW_OK;
}
