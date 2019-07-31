#include "mysw.h"

int server_new(proxy_t *proxy, char *host, int port, server_t **out_server) {
    server_t *server;
    int efd;

    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        return MYSW_ERR;
    }

    server = calloc(1, sizeof(server_t));
    server->host = strdup(host);
    server->port = port;

    /* Socket events (mysqld socket is readable or writeable) */
    server->fdh_conn.proxy = proxy;
    server->fdh_conn.type = FDH_TYPE_SERVER;
    server->fdh_conn.u.server = server;
    server->fdh_conn.fn_read_write = NULL;
    server->fdh_conn.fn_process = server_process;
    server->fdh_conn.fn_destroy = server_destroy;
    server->fdh_conn.fd = -1;
    server->fdh_conn.epoll_flags = EPOLLIN;

    /* Application events (e.g., wake-up to process a command) */
    server->fdh_event.proxy = proxy;
    server->fdh_event.type = FDH_TYPE_SERVER;
    server->fdh_event.u.server = server;
    server->fdh_event.fn_read_write = NULL;
    server->fdh_event.fn_process = server_process;
    server->fdh_event.fn_destroy = server_destroy;
    server->fdh_event.fd = efd;
    server->fdh_event.epoll_flags = EPOLLIN;

    server->state = SERVER_STATE_DISCONNECTED;

    if (out_server) *out_server = server;

    return MYSW_OK;
}

int server_process(fdh_t *fdh) {
    int rv;
    server_t *server;

    server = fdh->u.server;
    if (server->fdh_conn.read_eof || server->fdh_conn.read_write_errno) {
        /* The server disconnected or there was a read/write error */
        fdh->destroy = 1;
        return MYSW_OK;
    }

    switch (server->state) {
        case SERVER_STATE_CONNECTING:
            rv = server_process_connecting(server);
            break;
        case SERVER_STATE_CONNECTED:
            rv = server_process_connected(server);
            break;
        case SERVER_STATE_HANDSHAKE_SENDING_INIT:
            rv = server_process_handshake_sending_init(server);
            break;
        case SERVER_STATE_HANDSHAKE_SENT_INIT:
            rv = server_process_handshake_sent_init(server);
            break;
        default:
            fprintf(stderr, "server_process: Invalid server state %d\n", server->state);
            rv = MYSW_ERR;
            break;
    }
    return rv;
}

int server_connect(server_t *server) {
    int sockfd, sock_flags;
    int rv;
    struct sockaddr_in addr;

    /* Make socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("server_process_connecting: socket");
        return MYSW_ERR;
    }

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

    if (rv == -1 && errno != EINPROGRESS) {
        perror("server_new: connect");
        close(sockfd); /* TODO handle connection error (retry later?) */
        return MYSW_ERR;
    } else if (rv == -1 && errno == EINPROGRESS) {
        /* Socket now connecting */
        server->state = SERVER_STATE_CONNECTING;
        server->fdh_conn.fd = sockfd;
        /* From connect(2) EINPROGRESS. (It is possible to select(2) or poll(2)
         * for completion by selecting the socket for writing...) */
        server->fdh_conn.epoll_flags = EPOLLOUT;
        server->fdh_conn.fn_read_write = NULL;
        fdh_watch(&server->fdh_conn); /* TODO handle connection error (retry later?) */
        return MYSW_OK;
    } else if (rv == 0) {
        /* Non-blocking socket immediately connected? */
        server->state = SERVER_STATE_CONNECTED;
        server->fdh_conn.fd = sockfd;
        server->fdh_conn.epoll_flags = EPOLLIN; /* Expect handshake init from mysqld */
        server->fdh_conn.fn_read_write = fdh_read_write;
        fdh_watch(&server->fdh_conn); /* TODO handle connection error (retry later?) */
        return MYSW_OK;
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
    if (getsockopt(server->fdh_conn.fd, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) != 0) {
        perror("server_process_connecting: getsockopt");
        return MYSW_ERR;
    }

    if (sock_error != 0) {
        /* TODO handle connection error (retry later?) */
        fprintf(stderr, "server_process_connecting: async connect: %s\n", strerror(sock_error));
        return MYSW_ERR;
    }

    server->state = SERVER_STATE_CONNECTED;
    server->fdh_conn.fn_read_write = fdh_read_write;
    server->fdh_conn.epoll_flags = EPOLLIN; /* Expect handshake init from mysqld */
    return MYSW_OK;
}

int server_process_connected(server_t *server) {
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

    in = &server->fdh_conn.in;
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
    fdh_reset_rw_state(&server->fdh_conn);

    capability_flags = MYSQLD_CLIENT_PLUGIN_AUTH \
        | MYSQLD_CLIENT_SECURE_CONNECTION \
        | MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA \
        | MYSQLD_CLIENT_PROTOCOL_41;

    out = &server->fdh_conn.out;
    buf_clear(out);
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

    server->state = SERVER_STATE_HANDSHAKE_SENDING_INIT;
    server->fdh_conn.epoll_flags = EPOLLOUT;

    return MYSW_OK;
}

int server_process_handshake_sending_init(server_t *server) {
    fdh_t *fdh_conn;

    fdh_conn = &server->fdh_conn;

    if (fdh_is_write_finished(fdh_conn)) {
        /* Finished writing handshake packet. Transition state. */
        fdh_reset_rw_state(fdh_conn);
        server->state = SERVER_STATE_HANDSHAKE_SENT_INIT;
        server->fdh_conn.epoll_flags = EPOLLIN; /* poll for readable client */
    }

    return MYSW_OK;
}

int server_process_handshake_sent_init(server_t *server) {
    buf_t *in;
    size_t cur;
    uint32_t payload_len;
    uint8_t sequence_id;

    in = &server->fdh_conn.in;

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

    server->state = SERVER_STATE_COMMAND_READY;
    server->fdh_conn.epoll_flags = 0;

    return MYSW_OK;
}

int server_destroy(fdh_t *fdh) {
    server_t *server;
    server = fdh->u.server;
    free(server->host);
    free(server);
    return MYSW_OK;
}
