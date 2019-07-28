#include "mysw.h"

int fdh_watch(fdh_t *fdh) {
    int rv;
    struct epoll_event ev;

    /* Init epoll_event */
    memset(&ev, 0, sizeof(ev));
    #ifdef EPOLLEXCLUSIVE
        ev.events = EPOLLET | EPOLLEXCLUSIVE | fdh->epoll_flags;
    #else
        ev.events = EPOLLET | EPOLLONESHOT | fdh->epoll_flags;
    #endif
    ev.data.ptr = fdh;

    /* Add to epoll */
    if ((rv = epoll_ctl(fdh->proxy->epfd, EPOLL_CTL_ADD, fdh->fd, &ev)) < 0) {
        perror("fdh_watch: epoll_ctl");
    }

    return rv;
}

int fdh_rewatch(fdh_t *fdh) {
    #ifdef EPOLLEXCLUSIVE
        (void)fdh;
        return 0;
    #else
        return fdh_watch(fdh);
    #endif
}

int fdh_unwatch(fdh_t *fdh) {
    #ifdef EPOLLEXCLUSIVE
        struct epoll_event ev;
        return epoll_ctl(fdh->proxy->epfd, EPOLL_CTL_DEL, fdh->fd, &ev);
    #else
        return 0;
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

int fdh_write(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *out;
    char *buf;

    out = &fdh->out;

    len = out->len - fdh->out_cur;
    buf = out->data + fdh->out_cur;

    rv = write(fdh->fd, buf, len);
    if (rv == -1) {
        /* Write error */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* Try again later */
            rv = 0;
        } else {
            /* Fatal error */
            fdh->last_errno = errno;
        }
    } else if (rv < (ssize_t)len) {
        /* Partial write */
        fdh->out_cur += rv;
    } else {
        /* Finished write */
        fdh->out_cur = len;
    }
    return (int)rv;
}

int fdh_is_write_finished(fdh_t *fdh) {
    if (fdh->out_cur >= fdh->out.len) {
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
    fdh->out_cur = 0;
    fdh->last_errno = 0;
    fdh->eof = 0;
    return 0;
}

int fdh_read(fdh_t *fdh) {
    ssize_t rv;
    size_t len;
    buf_t *in;
    char *buf;

    in = &fdh->in;

    len = in->len;
    buf = in->data + len;
    buf_ensure_cap(in, len + opt_read_size + 1);

    rv = read(fdh->fd, buf, opt_read_size);
    if (rv == -1) {
        /* Read error */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* Try again later */
            rv = 0;
        } else {
            /* Fatal error */
            fdh->last_errno = errno;
        }
    } else if (rv > 0) {
        /* Read success */
        in->len += rv;
        *(in->data + in->len) = '\0';
    } else {
        /* Read EOF */
        fdh->eof = 1;
    }
    return (int)rv;
}
