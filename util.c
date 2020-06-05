#include "mysw.h"

int epoll_add_or_mod(int epollfd, int fd, struct epoll_event *event) {
    int rv;
    rv = epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, event);
    if (rv != 0 && errno == EEXIST) {
        rv = epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, event);
    }
    return rv;
}
