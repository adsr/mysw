#include "mysw.h"

static int server_set_state(server_t *server, int state, fdh_t *fdh);

int server_new(proxy_t *proxy, char *host, int port, server_t **out_server) {
    server_t *server;
    int efd, tfd;

    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        return MYSW_ERR;
    }

    if ((tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
        close(efd);
        return MYSW_ERR;
    }

    server = calloc(1, sizeof(server_t));

    pthread_spin_init(&server->spinlock, PTHREAD_PROCESS_PRIVATE);
    /* TODO pthread_spin_destroy */

    server->proxy = proxy;
    server->fdh_socket_in.fd = -1;
    server->host = strdup(host);
    server->port = port;

    /* Init application event handle (internal eventfd) */
    fdh_init(&server->fdh_event, NULL, proxy->fdpoll, server, &server->spinlock, FDH_TYPE_EVENT, efd, server_process);

    /* Init application timer handle (internal timerfd) */
    fdh_init(&server->fdh_timer, NULL, proxy->fdpoll, server, &server->spinlock, FDH_TYPE_EVENT, tfd, server_process);

    if (out_server) *out_server = server;

    return MYSW_OK;
}


int server_process(fdh_t *fdh) {
    int rv;
    server_t *server;

    server = fdh->udata;

    if (server->fdh_socket_in.read_eof || server->fdh_socket_in.read_write_errno) {
        /* The server disconnected or there was a read/write error */
        /* TODO mark for destruction instead of destroying */
        /* TODO write async event that loops until refcount==0 to safely destroy the server */
        /* TODO server_destroy(server); */
        return server_set_state(server, SERVER_STATE_DISCONNECTED, NULL);
    }

    switch (server->state) {
        case SERVER_STATE_DISCONNECTED:            rv = server_process_disconnected(server); break;
        case SERVER_STATE_CONNECT:                 rv = server_process_connect(server); break;
        case SERVER_STATE_CONNECTING:              rv = server_process_connecting(server); break;
        case SERVER_STATE_SEND_HANDSHAKE_INIT:     rv = server_process_send_handshake_init(server); break;
        case SERVER_STATE_RECV_HANDSHAKE_INIT_RES: rv = server_process_recv_handshake_init_res(server); break;
        case SERVER_STATE_SEND_HANDSHAKE_RES:      rv = server_process_send_handshake_res(server); break;
        case SERVER_STATE_WAIT_CLIENT:             rv = server_process_wait_client(server); break;
        case SERVER_STATE_RECV_CMD:                rv = server_process_recv_cmd(server); break;
        case SERVER_STATE_SEND_CMD_RES:            rv = server_process_send_cmd_res(server); break;
        default: fprintf(stderr, "server_process: Invalid server state %d\n", server->state); rv = MYSW_ERR; break;
    }

    return rv;
}

int server_process_disconnected(server_t *server) {
    struct itimerspec it;

    /* Close existing socket */
    if (server->fdh_socket_in.fd >= 0) {
        close(server->fdh_socket_in.fd);
        server->fdh_socket_in.fd = -1;
    }

    /* Set timer for reconnect */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_nsec = 0;
    it.it_value.tv_sec = 1; /* TODO backoff? */
    it.it_value.tv_nsec = 0;
    timerfd_settime(server->fdh_timer.fd, 0, &it, NULL);

    return server_set_state(server, SERVER_STATE_CONNECT, &server->fdh_timer);
}

int server_process_connect(server_t *server) {
    int sockfd, sock_flags;
    int rv;
    struct sockaddr_in addr;

    /* Make socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("server_process_connecting: socket");
        return MYSW_ERR;
    }

    /* Init socket event handle (network io) */
    fdh_init(&server->fdh_socket_in, &server->fdh_socket_out, server->proxy->fdpoll, server, &server->spinlock, FDH_TYPE_SOCKET, sockfd, server_process);

    /* Set non-blocking mode */
    if ((sock_flags = fcntl(sockfd, F_GETFL, 0)) < 0 || fcntl(sockfd, F_SETFL, sock_flags | O_NONBLOCK) < 0) {
        perror("server_process_connecting: fcntl");
        close(sockfd);
        return MYSW_ERR;
    }

    /* Connect */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(server->host); /* TODO name lookup */
    addr.sin_port = htons(server->port);
    rv = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));

    if (rv == 0) {
        /* Non-blocking socket immediately connected? */
        perror("server_new: connect");
        close(sockfd); /* TODO handle connection error (retry later?) */
        return MYSW_ERR;
    } else if (rv < 0 && errno != EINPROGRESS) {
        perror("server_new: connect");
        close(sockfd); /* TODO handle connection error (retry later?) */
        return MYSW_ERR;
    } else if (rv < 0 && errno == EINPROGRESS) {
        /* Socket now connecting */
        /* From connect(2) EINPROGRESS. (It is possible to select(2) or poll(2)
         * for completion by selecting the socket for writing...) */
        server->fdh_socket_out.read_write_skip = 1;
        return server_set_state(server, SERVER_STATE_CONNECTING, &server->fdh_socket_out);
    }

    return MYSW_ERR;
}

int server_process_connecting(server_t *server) {
    socklen_t sock_error_len;
    int sock_error;

    /* From connect(2) EINPROGRESS: "After select(2) indicates writability, use
     * getsockopt(2) to read the SO_ERROR option at level SOL_SOCKET to
     * determine whether connect() completed successfully (SO_ERROR is zero) or
     * unsuccessfully (SO_ERROR is one of the usual error codes listed here,
     * explaining the reason for the failure). */
    sock_error_len = sizeof(sock_error);
    if (getsockopt(server->fdh_socket_in.fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) != 0) {
        perror("server_process_connecting: getsockopt");
        return MYSW_ERR;
    }

    if (sock_error != 0) {
        /* TODO handle connection error (retry later?) */
        fprintf(stderr, "server_process_connecting: async connect: %s\n", strerror(sock_error));
        return MYSW_ERR;
    }

    server->fdh_socket_out.read_write_skip = 0;
    return server_set_state(server, SERVER_STATE_SEND_HANDSHAKE_INIT, &server->fdh_socket_in);
}

int server_process_send_handshake_init(server_t *server) {
    buf_t *in, *out;
    int payload_len, sequence_id, protocol_ver, conn_id;
    char *server_ver, *challenge_lo, *challenge_hi;
    int capability_flags, capability_flags_lo, capability_flags_hi;
    int status_flags;
    int charset;
    uchar challenge[20];
    uchar native_auth_response[SHA_DIGEST_LENGTH];
    size_t server_ver_len;
    size_t cur;
    char *plugin;
    size_t plugin_len;

    in = &server->fdh_socket_in.buf;
    if (!util_has_complete_mysql_packet(in)) {
        return MYSW_OK;
    }

    /* TODO ensure protocol version is 10 */
    /* TODO ensure capabilities has CLIENT_PLUGIN_AUTH */
    /* TODO ensure auth_plugin_name is mysql_native_password */
    /* TODO support other auth and/or send AuthSwitchRequest */

    /* https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_v10.html */
    cur = 0;
    payload_len = buf_get_u24(in, cur);                     cur += 3;
    sequence_id = buf_get_u8(in, cur);                      cur += 1;
    protocol_ver = buf_get_u8(in, cur);                     cur += 1;
    server_ver = buf_get_str0(in, cur, &server_ver_len);    cur += server_ver_len + 1;
    conn_id = buf_get_u32(in, cur);                         cur += 4;
    challenge_lo = buf_get_str(in, cur);                    cur += 8;
    cur += 1; /* filler */
    capability_flags_lo = buf_get_u16(in, cur);             cur += 2;
    charset = buf_get_u8(in, cur);                          cur += 1;
    status_flags = buf_get_u16(in, cur);                    cur += 2;
    capability_flags_hi = buf_get_u16(in, cur);             cur += 2;
    capability_flags = (capability_flags_lo & 0xffff) + ((capability_flags_hi & 0xffff) << 16);
    cur += 1; /* auth_plugin_data_len */
    cur += 10; /* filler (zeros) */
    challenge_hi = buf_get_str(in, cur);                    cur += 13;
    plugin = buf_get_str0(in, cur, &plugin_len);            cur += plugin_len + 1;

    (void)plugin;
    (void)charset;
    (void)status_flags;
    (void)server_ver;
    (void)conn_id;
    (void)protocol_ver;
    (void)payload_len;

    /* TODO actually do auth */
    memcpy(challenge,       challenge_lo, 8);
    memcpy(challenge + 8,   challenge_hi, 12);
    util_calc_native_auth_response((const uchar *)"testpass", strlen("testpass"), (const uchar *)challenge, native_auth_response);

    /* TODO reset fdh probably */

    capability_flags = MYSQLD_CLIENT_PLUGIN_AUTH \
        | MYSQLD_CLIENT_SECURE_CONNECTION \
        | MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA \
        | MYSQLD_CLIENT_PROTOCOL_41;

    fdh_reset_rw_state(&server->fdh_socket_out);
    out = &server->fdh_socket_out.buf;
    buf_append_str_len(out, "\x00\x00\x00", 3);                 /* payload len (3) (set below) */
    buf_append_u8(out, sequence_id + 1);                        /* sequence id (1) */
    buf_append_u32(out, capability_flags);                      /* capability flags (4) */
    buf_append_u32(out, 0xffffffff);                            /* max packet size (4) */
    buf_append_u8(out, '\x21');                                 /* charset (1) */
    buf_append_u8_repeat(out, '\x00', 23);                      /* filler */
    buf_append_str_len(out, "testuser\x00", 9);                 /* username */
    /* TODO buf_append/get_str_lenenc https://dev.mysql.com/doc/internals/en/integer.html#length-encoded-integer */
    buf_append_u8(out, SHA_DIGEST_LENGTH);                      /* native_auth_response lenenc */
    buf_append_str_len(out, (char *)native_auth_response, SHA_DIGEST_LENGTH);
    buf_append_str_len(out, "mysql_native_password\x00", 22);   /* client_plugin_name */
    buf_set_u24(out, 0, buf_len(out) - 4);                      /* set payload len */

    return server_set_state(server, SERVER_STATE_RECV_HANDSHAKE_INIT_RES, &server->fdh_socket_out);
}

int server_process_recv_handshake_init_res(server_t *server) {
    fdh_t *fdh_socket;

    fdh_socket = &server->fdh_socket_out;

    if (fdh_is_write_finished(fdh_socket)) {
        /* Finished writing handshake packet. Transition state. */
        fdh_reset_rw_state(&server->fdh_socket_in);
        return server_set_state(server, SERVER_STATE_SEND_HANDSHAKE_RES, &server->fdh_socket_in);
    }

    return MYSW_OK;
}

int server_process_send_handshake_res(server_t *server) {
    buf_t *in;
    size_t cur;
    uint32_t payload_len;
    uint8_t sequence_id;

    in = &server->fdh_socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        return MYSW_OK;
    }

    cur = 0;
    payload_len = buf_get_u24(in, cur);     cur += 3;
    sequence_id = buf_get_u8(in, cur);      cur += 1;

    (void)sequence_id;

    if (payload_len < 1 || buf_get_u8(in, cur) != MYSQLD_OK) {
        /* TODO error */
        return MYSW_ERR;
    }

    return server_set_state(server, SERVER_STATE_WAIT_CLIENT, &server->fdh_event);
}

int server_process_wait_client(server_t *server) {
    buf_t *out;

    /* TODO assert server->target_client */

    fdh_reset_rw_state(&server->fdh_socket_out);
    out = &server->fdh_socket_out.buf;
    buf_clear(out);
    buf_copy_from(out, server->target_client->cmd.payload);

    return server_set_state(server, SERVER_STATE_RECV_CMD, &server->fdh_socket_out);
}

int server_process_recv_cmd(server_t *server) {
    fdh_t *fdh_socket;

    fdh_socket = &server->fdh_socket_out;

    if (fdh_is_write_finished(fdh_socket)) {
        /* Finished writing handshake packet. Transition state. */
        fdh_reset_rw_state(&server->fdh_socket_in);
        return server_set_state(server, SERVER_STATE_SEND_CMD_RES, &server->fdh_socket_in);
    }

    return MYSW_OK;
}

int server_process_send_cmd_res(server_t *server) {
    buf_t *in;

    in = &server->fdh_socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        return MYSW_OK;
    }

    buf_copy_from(&server->target_client->cmd_result, in);
    client_wakeup(server->target_client);

    fdh_reset_rw_state(&server->fdh_socket_in);

    return server_set_state(server, SERVER_STATE_WAIT_CLIENT, &server->fdh_event);
}

int server_set_client(server_t *server, client_t *client) {
    return MYSW_ERR;
}

int server_wakeup(server_t *server) {
    uint64_t i;
    i = 1;
    return (write(server->fdh_event.fd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}

int server_destroy(fdh_t *fdh) {
    server_t *server;
    server = fdh->udata;
    free(server->host);
    free(server);
    return MYSW_OK;
}

static int server_set_state(server_t *server, int state, fdh_t *fdh) {
    int rv;

    /* Unwatch the other fdhs */
    if (fdh != &server->fdh_socket_in)  try(rv, fdh_ensure_unwatched(&server->fdh_socket_in));
    if (fdh != &server->fdh_socket_out) try(rv, fdh_ensure_unwatched(&server->fdh_socket_out));
    if (fdh != &server->fdh_event)      try(rv, fdh_ensure_unwatched(&server->fdh_event));
    if (fdh != &server->fdh_timer)      try(rv, fdh_ensure_unwatched(&server->fdh_timer));

    /* Watch the specified fdh */
    if (fdh) fdh_ensure_watched(fdh);

    /* Set state */
    server->state = state;
    return MYSW_OK;
}
