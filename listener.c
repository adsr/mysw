#include "mysw.h"

int listener_create() {
    struct sockaddr_in addr;
    int optval;

    // create socket
    if ((mysw.listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("create_listener: socket");
        return MYSW_ERR;
    }

    // set SO_REUSEPORT
    optval = 1;
    if ((setsockopt(mysw.listenfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval))) < 0) {
        perror("create_listener: setsockopt");
        return MYSW_ERR;
    }

    // bind to port
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = mysw.opt_addr ? inet_addr(mysw.opt_addr) : INADDR_ANY;
    addr.sin_port = htons(mysw.opt_port);
    if (bind(mysw.listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("create_listener: bind");
        return MYSW_ERR;
    }

    // listen
    if (listen(mysw.listenfd, mysw.opt_backlog) < 0) {
        perror("create_listener: listen");
        return MYSW_ERR;
    }

    return MYSW_OK;
}

int listener_free() {
    if (mysw.listenfd >= 0) {
        close(mysw.listenfd);
    }
    return MYSW_OK;
}
