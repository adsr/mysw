#include "mysw.h"

int targeter_new(proxy_t *proxy, targeter_t **out_targeter) {
    targeter_t *targeter;

    targeter = calloc(1, sizeof(targeter_t));
    targeter->proxy = proxy;
    targeter->fdh_socket_in.fd = -1;
    pthread_spin_init(&targeter->spinlock, PTHREAD_PROCESS_PRIVATE);

    targeter_connect(targeter);

    *out_targeter = targeter;
    return MYSW_OK;
}

int targeter_process(fdh_t *fdh) {
    int rv;
    targeter_t *targeter;
    client_t *client, *client_tmp;

    targeter = fdh->udata;

    if (targeter->fdh_socket_in.read_eof
        || targeter->fdh_socket_in.read_write_errno
        || targeter->fdh_socket_out.read_write_errno
    ) {
        /* Send error to all pending clients */
        HASH_ITER(hh_in_targeter, targeter->client_map, client, client_tmp) {
            client_write_err_packet(client, "targeter disconnected");
            HASH_DELETE(hh_in_targeter, targeter->client_map, client);
        }

        /* Reinit targeter */
        targeter_deinit(targeter);
        if (targeter_connect(targeter) != MYSW_OK) {
            /* TODO reconnect backoff */
        }

        return MYSW_OK;
    }

    if (targeter->state == TARGETER_STATE_CONNECTING && fdh == &targeter->fdh_socket_out) {
        rv = targeter_process_connecting(targeter);
    } else if (targeter->state == TARGETER_STATE_READ && fdh == &targeter->fdh_socket_in) {
        rv = targeter_process_read(targeter);
    } else {
        fprintf(stderr, "targeter_process: Invalid targeter state %d\n", targeter->state);
        rv = MYSW_ERR;
    }

    return rv;
}

int targeter_process_read(targeter_t *targeter) {
    buf_t *buf;
    size_t len, pos, response_len, poolname_len;
    uint64_t client_id, request_id;
    char *poolname;
    pool_t *pool;
    client_t *client;

    buf = &targeter->buf;

    /* BEGIN parse response */
    len = buf_len(buf);
    if (len < 4) return MYSW_OK;
    pos = 0;
    response_len = buf_get_u32(buf, pos);   pos += 4;
    if (len - 4 < response_len) return MYSW_OK;
    request_id = buf_get_u64(buf, pos);     pos += 8;
    client_id = buf_get_u64(buf, pos);      pos += 8;
    poolname_len = buf_get_u16(buf, pos);   pos += 2;
    poolname = (char*)buf_get(buf, pos);           pos += poolname_len;
    /* END parse response */

    /* Look up client */
    HASH_FIND(hh_in_targeter, targeter->client_map, &client_id, sizeof(client_id), client);
    if (!client) {
        /* TODO not found? */
        return MYSW_ERR;
    } else if (client->request_id != request_id) {
        /* TODO different client? */
        return MYSW_ERR;
    }

    /* Find pool */
    pool_find(targeter->proxy, poolname, poolname_len, &pool);
    if (!pool) {
        /* TODO pool not found */
        return MYSW_ERR;
    }

    /* Queue on pool */
    pool_queue_client(pool, client);
    pool_wakeup(pool);

    /* Clear buf */
    buf_clear(buf);

    return MYSW_OK;
}

int targeter_queue_client(targeter_t *targeter, client_t *client) {
    buf_t *buf;
    cmd_t *cmd;

    pthread_spin_lock(&targeter->spinlock);

    buf = &targeter->fdh_socket_out.buf;
    cmd = &client->cmd;

    /* TODO url encode / json / msgpack instead of binary protocol? configurable? */

    /* BEGIN build request */
    buf_append_u32(buf, 0);
    buf_append_u64(buf, client->client_id);
    buf_append_u8(buf, cmd->cmd_byte);
    buf_append_u16(buf, buf_len(&client->db_name));  buf_append_buf(buf, &client->db_name);
    buf_append_u16(buf, buf_len(&client->hint));     buf_append_buf(buf, &client->hint);
    buf_append_u16(buf, buf_len(&client->username)); buf_append_buf(buf, &client->username);
    buf_set_u32(buf, 0, buf_len(buf) - 4);
    /* END build request */

    HASH_ADD(hh_in_targeter, targeter->client_map, client_id, sizeof(client->client_id), client);

    fdh_ensure_watched(&targeter->fdh_socket_out);

    pthread_spin_unlock(&targeter->spinlock);
    return MYSW_OK;
}

int targeter_connect(targeter_t *targeter) {
    int sockfd, sock_flags;
    int rv;
    struct sockaddr_un addr;

    /* Make socket */
    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("targeter_connect: socket");
        return MYSW_ERR;
    }

    /* Init socket event handle (network io) */
    fdh_init(&targeter->fdh_socket_in, &targeter->fdh_socket_out, targeter->proxy->fdpoll, targeter, &targeter->spinlock, FDH_TYPE_SOCKET, sockfd, targeter_process);
    targeter->fdh_socket_out.fn_process = NULL;

    /* Set non-blocking mode */
    if ((sock_flags = fcntl(sockfd, F_GETFL, 0)) < 0 || fcntl(sockfd, F_SETFL, sock_flags | O_NONBLOCK) < 0) {
        perror("targeter_connect: fcntl");
        close(sockfd);
        return MYSW_ERR;
    }

    /* Connect */
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "TODO not this anymore");
    rv = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    if (rv < 0 && errno != EINPROGRESS) {
        perror("targeter_connect: connect");
        close(sockfd); /* TODO handle connection error (retry later?) */
        return MYSW_ERR;
    } else if (rv == 0) {
        targeter->state = TARGETER_STATE_READ;
        try(rv, fdh_ensure_watched(&targeter->fdh_socket_in));
        try(rv, fdh_ensure_unwatched(&targeter->fdh_socket_out));
        return MYSW_OK;
    } else if (rv < 0 && errno == EINPROGRESS) {
        /* Socket now connecting */
        /* From connect(2) EINPROGRESS. (It is possible to select(2) or poll(2)
         * for completion by selecting the socket for writing...) */
        targeter->fdh_socket_out.read_write_skip = 1;
        targeter->state = TARGETER_STATE_CONNECTING;
        try(rv, fdh_ensure_unwatched(&targeter->fdh_socket_in));
        try(rv, fdh_ensure_watched(&targeter->fdh_socket_out));
        return MYSW_OK;
    }

    return MYSW_ERR;
}

int targeter_process_connecting(targeter_t *targeter) {
    int rv;
    socklen_t sock_error_len;
    int sock_error;

    /* From connect(2) EINPROGRESS: "After select(2) indicates writability, use
     * getsockopt(2) to read the SO_ERROR option at level SOL_SOCKET to
     * determine whether connect() completed successfully (SO_ERROR is zero) or
     * unsuccessfully (SO_ERROR is one of the usual error codes listed here,
     * explaining the reason for the failure). */
    sock_error_len = sizeof(sock_error);
    if (getsockopt(targeter->fdh_socket_in.fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) != 0) {
        perror("targeter_process_connecting: getsockopt");
        /* TODO reconnect backoff */
        return MYSW_ERR;
    }

    if (sock_error != 0) {
        /* TODO reconnect backoff */
        fprintf(stderr, "targeter_process_connecting: async connect: %s\n", strerror(sock_error));
        return MYSW_ERR;
    }

    targeter->fdh_socket_out.read_write_skip = 0;
    targeter->state = TARGETER_STATE_READ;
    try(rv, fdh_ensure_watched(&targeter->fdh_socket_in));
    return MYSW_OK;
}


int targeter_deinit(targeter_t *targeter) {
    if (targeter->fdh_socket_in.fd != -1) {
        close(targeter->fdh_socket_in.fd);
        fdh_deinit(&targeter->fdh_socket_in, &targeter->fdh_socket_out);
    }
    return MYSW_OK;
}

int targeter_free(targeter_t *targeter) {
    targeter_deinit(targeter);
    pthread_spin_destroy(&targeter->spinlock);
    free(targeter);
    return MYSW_OK;
}
