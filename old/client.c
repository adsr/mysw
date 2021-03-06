#include "mysw.h"

static int client_process_recv_handshake_init(client_t *client);
static int client_process_send_handshake_init_res(client_t *client);
static int client_process_recv_handshake_res(client_t *client);
static int client_process_send_cmd(client_t *client);
static int client_process_send_cmd_inner(client_t *client);
static int client_process_wait_cmd_res(client_t *client);
static int client_process_recv_cmd_res(client_t *client);

int client_new(proxy_t *proxy, int sockfd, client_t **out_client) {
    client_t *client;
    int efd;

    /* Make event fd */
    if ((efd = eventfd(0, EFD_NONBLOCK)) < 0) {
        perror("client_new: eventfd");
        return MYSW_ERR;
    }

    client = calloc(1, sizeof(client_t));
    client->proxy = proxy;
    pthread_spin_init(&client->spinlock, PTHREAD_PROCESS_PRIVATE);

    fdo_init(&client->fdo, proxy->fdpoll, client, &client->state, &client->spinlock, sockfd, efd, -1, client_process);

    fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_INIT, FDH_TYPE_SOCKET_OUT);

    if (out_client) *out_client = client;

    return MYSW_OK;
}

int client_destroy(client_t *client) {
    close(client->fdo.socket_in.fd);
    close(client->fdo.event_in.fd);
    fdo_deinit(&client->fdo);
    /* TODO avoid use after free, e.g., other objects may have client pointer */
    free(client);
    return MYSW_OK;
}

int client_process(fdh_t *fdh) {
    client_t *client;
    int rv;

    client = fdh->fdo->udata;

    if (client->fdo.socket_in.read_eof || client->fdo.socket_in.read_write_errno || client->fdo.socket_out.read_write_errno) {
        /* The client disconnected or there was a read/write error */
        if (client->target_server) {
            /* TODO assert client->target_server->in_reserved */
            pool_server_move_to_free(client->target_server->pool, client->target_server);
        }
        /* TODO refcounting for destroying objects */
        return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_INIT, 0);
    }

    switch (client->state) {
        case CLIENT_STATE_RECV_HANDSHAKE_INIT:     rv = client_process_recv_handshake_init(client); break;
        case CLIENT_STATE_SEND_HANDSHAKE_INIT_RES: rv = client_process_send_handshake_init_res(client); break;
        case CLIENT_STATE_RECV_HANDSHAKE_RES:      rv = client_process_recv_handshake_res(client); break;
        case CLIENT_STATE_SEND_CMD:                rv = client_process_send_cmd(client); break;
        case CLIENT_STATE_WAIT_CMD_RES:            rv = client_process_wait_cmd_res(client); break;
        case CLIENT_STATE_RECV_CMD_RES:            rv = client_process_recv_cmd_res(client); break;
        default: fprintf(stderr, "client_process: Invalid client state %d\n", client->state); rv = MYSW_ERR; break;
    }

    return rv;
}

/**
 * The client just connected. Send an Initial Handshake Packet.
 * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase.html
 */
static int client_process_recv_handshake_init(client_t *client) {
    uint32_t capability_flags;
    int i;
    uchar challenge[21];
    buf_t *out;
    fdh_t *socket_out;

    socket_out = &client->fdo.socket_out;

    if (!fdh_is_writing(socket_out)) {
        /* Queue handshake packet */

        /* Init challenge */
        for (i = 0; i < 20; ++i) challenge[i] = (uchar)rand();
        challenge[20] = '\x00';

        /* Init caps */
        capability_flags = MYSQLD_CLIENT_PLUGIN_AUTH \
            | MYSQLD_CLIENT_SECURE_CONNECTION \
            | MYSQLD_CLIENT_CONNECT_WITH_DB \
            | MYSQLD_CLIENT_PROTOCOL_41;

        /* Build init packet */
        /* TODO support other caps, auth, ssl */
        fdh_clear(socket_out);
        out = &socket_out->buf;
        buf_clear(out);
        buf_append_u24(out, 0);                                 /* payload len (3) (set below) */
        buf_append_u8(out, '\x00');                             /* sequence id (1) */
        buf_append_u8(out, '\x0a');                             /* protocol version */
        buf_append_str_len(out, "derp\x00", 5);                 /* TODO server version */
        buf_append_str_len(out, "\x00\x00\x00\x00", 4);         /* TODO connection id */
        buf_append_str_len(out, (char *)challenge, 8);          /* challenge[:8] */
        buf_append_u8(out, '\x00');                             /* filler */
        buf_append_u16(out, capability_flags & 0xffff);         /* cap flags lower 2 */
        buf_append_u8(out, '\x21');                             /* TODO character set */
        buf_append_u16(out, 0x0002);                            /* TODO status flags */
        buf_append_u16(out, (capability_flags >> 16) & 0xffff); /* cap flags upper 2 */
        buf_append_u8(out, 21);                                 /* len(challenge) */
        buf_append_u8_repeat(out, '\x00', 10);                  /* filler */
        buf_append_str_len(out, (char *)(challenge + 8), 13);   /* challenge[8:] */
        buf_append_str_len(out, "mysql_native_password\x00", 22);
        buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */
    }

    if (fdh_is_write_finished(socket_out)) {
        /* Finished writing; transition state */
        return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_HANDSHAKE_INIT_RES, FDH_TYPE_SOCKET_IN);
    }

    /* Not yet finished writing; re-arm */
    return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_INIT, FDH_TYPE_SOCKET_OUT);
}

/* The client finished receiving the handshake packet. We now expect the client
 * to send a response including auth. Then we reply with either an OK or an ERR
 * packet, and the connection moves into the command phase.
 */
static int client_process_send_handshake_init_res(client_t *client) {
    buf_t *in;
    size_t cur;
    int capability_flags;
    int max_packet_size;
    int charset;
    uint8_t sequence_id;
    int payload_len;
    char *username, *auth, *db_name, *plugin;
    size_t username_len, auth_len, db_name_len, plugin_len;

    in = &client->fdo.socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        /* Have not received a complete packet yet; re-arm */
        return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_HANDSHAKE_INIT_RES, FDH_TYPE_SOCKET_IN);
    }

    /* We have a complete packet; parse and respond */
    /* https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_response.html */
    cur = 0;
    payload_len = buf_get_u24(in, cur);                 cur += 3;
    sequence_id = buf_get_u8(in, cur);                  cur += 1;
    capability_flags = buf_get_u32(in, cur);            cur += 4;
    max_packet_size = buf_get_u32(in, cur);             cur += 4;
    charset = buf_get_u8(in, cur);                      cur += 1;
    cur += 23; /* filler */
    username = buf_get_str0(in, cur, &username_len);    cur += username_len + 1;
    buf_assign_str_len(&client->username, username, username_len);
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        auth_len = buf_get_u8(in, cur);                 cur += 1;
        auth = buf_get_str(in, cur);                    cur += auth_len;
    } else {
        /* TODO not clear what to expect here */
        auth = buf_get_str0(in, cur, &auth_len);        cur += auth_len;
    }
    if (capability_flags & MYSQLD_CLIENT_CONNECT_WITH_DB) {
        db_name = buf_get_str0(in, cur, &db_name_len);  cur += db_name_len + 1;
        buf_assign_str_len(&client->db_name, db_name, db_name_len);
    }
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH) {
        plugin = buf_get_str0(in, cur, &plugin_len);    cur += plugin_len + 1;
    }

    if (strncmp(plugin, "mysql_native_password", plugin_len) != 0) {
        /* TODO error */
        return MYSW_ERR;
    }

    /* TODO actually auth */
    /* TODO queue ERR packet if failed auth */
    (void)username;
    (void)auth;
    (void)db_name;
    (void)plugin;
    (void)charset;
    (void)max_packet_size;
    (void)payload_len;

    /* Queue OK packet */
    client->status_flags = MYSQLD_SERVER_STATUS_AUTOCOMMIT;
    client->last_sequence_id = sequence_id;
    client_write_ok_packet(client);

    /* Transition state */
    return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_RES, FDH_TYPE_SOCKET_OUT);
}

static int client_process_recv_handshake_res(client_t *client) {
    if (!fdh_is_write_finished(&client->fdo.socket_out)) {
        /* We have not finished writing to the client; re-arm */
        return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_RES, FDH_TYPE_SOCKET_OUT);
    }

    /* We have finished writing to the client; transition state */
    fdh_clear(&client->fdo.socket_in);
    return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_CMD, FDH_TYPE_SOCKET_IN);
}


static int client_process_send_cmd(client_t *client) {
    buf_t *in;

    in = &client->fdo.socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        /* Client has not sent a complete packet yet; re-arm */
        return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_CMD, FDH_TYPE_SOCKET_IN);
    }

    client->last_sequence_id = buf_get_u8(in, 3);
    cmd_init(client, in, &client->cmd);

    client->request_id = 1; /* TODO random number or atomic incr */

    switch (client->cmd.cmd_byte) {
        case MYSQLD_COM_QUIT:
            /* TODO client destroy */
            return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_HANDSHAKE_INIT, 0);
        case MYSQLD_COM_STMT_PREPARE:
            client->prep_stmt_count += 1;
            break;
        case MYSQLD_COM_STMT_CLOSE:
            client->prep_stmt_count -= 1;
            break;
        case MYSQLD_COM_INIT_DB:
            client_set_db_name(client, (char*)in->data + 5, in->len - 5);
            client_write_ok_packet(client);
            return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_CMD_RES, FDH_TYPE_SOCKET_OUT);
    }

    return client_process_send_cmd_inner(client);
}

int client_write_ok_packet(client_t *client) {
    buf_t *out;

    /* Queue OK packet */
    fdh_clear(&client->fdo.socket_out);
    out = &client->fdo.socket_out.buf;
    buf_clear(out);
    buf_append_u24(out, 0);                                 /* payload len (3) (set below) */
    buf_append_u8(out, client->last_sequence_id + 1);       /* sequence id (1) */
    buf_append_u8(out, '\x00');                             /* OK packet header */
    buf_append_u8(out, '\x00');                             /* affected rows (lenenc) */
    buf_append_u8(out, '\x00');                             /* last insert id (lenenc) */
    buf_append_u16(out, client->status_flags);              /* server status flags (2) */
    buf_append_u16(out, 0);                                 /* warnings (2) */
    buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */
    return MYSW_OK;
}

int client_write_err_packet(client_t *client, const char *err_fmt, ...) {
    buf_t *out;
    char err[1024];
    va_list vl;

    va_start(vl, err_fmt);
    vsnprintf(err, sizeof(err), err_fmt, vl);
    va_end(vl);

    /* Queue ERR packet */
    fdh_clear(&client->fdo.socket_out);
    out = &client->fdo.socket_out.buf;
    buf_clear(out);
    buf_append_u24(out, 0);                                 /* payload len (3) (set below) */
    buf_append_u8(out, client->last_sequence_id + 1);       /* sequence id (1) */
    buf_append_u8(out, '\xff');                             /* ERR packet header */
    buf_append_u16(out, 0);                                 /* error code (2) */
    buf_append_u8(out, '#');                                /* sql_state_marker (1) */
    buf_append_str_len(out, "HY000", 5);                    /* sql_state (5) */
    buf_append_str(out, err);                               /* error message (eof) */
    buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */
    return MYSW_OK;
}

static int client_process_send_cmd_inner(client_t *client) {
    if (client->target_server) {
        /* Already have a reserved mysqld */
        /* TODO assert client->target_server->in_reserved */
        /* TODO assert client->target_server->in_txn || client->prep_stmt_count > 0 */
        server_wakeup(client->target_server);
    } else if (!client->target_pool || cmd_is_targeting(&client->cmd)) {
        /* Need to target */
        targeter_queue_client(client->proxy->targeter, client);
    } else {
        /* Use existing target_pool */
        pool_queue_client(client->target_pool, client);
        pool_wakeup(client->target_pool);
    }

    return fdo_set_state(&client->fdo, CLIENT_STATE_WAIT_CMD_RES, FDH_TYPE_EVENT_IN); /* TODO FDH_TYPE_SOCKET_IN for hangup? */
}

static int client_process_wait_cmd_res(client_t *client) {
    cmd_t *cmd;

    cmd = &client->cmd;

    if (!cmd_expects_response(cmd)) {
        /* No response is sent back to the client for this cmd */
        fdh_clear(&client->fdo.socket_in);
        return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_CMD, FDH_TYPE_SOCKET_IN);
    } else if (cmd->cmd_byte == MYSQLD_COM_QUERY && cmd->stmt_cur->next) {
        /* Process next stmt in multi-stmt cmd */
        /* TODO assert server sent MYSQLD_SERVER_MORE_RESULTS_EXISTS */
        cmd->stmt_cur = cmd->stmt_cur->next;
        return client_process_send_cmd_inner(client);
    }

    /* Time to send results. */
    fdh_clear(&client->fdo.socket_out);
    buf_copy_from(&client->fdo.socket_out.buf, &client->cmd_result);
    buf_clear(&client->cmd_result);
    return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_CMD_RES, FDH_TYPE_SOCKET_OUT);
}

static int client_process_recv_cmd_res(client_t *client) {
    fdh_t *socket_out;

    socket_out = &client->fdo.socket_out;

    if (fdh_is_write_finished(socket_out)) {
        /* Finished writing; transition state */
        fdh_clear(&client->fdo.socket_in);
        return fdo_set_state(&client->fdo, CLIENT_STATE_SEND_CMD, FDH_TYPE_SOCKET_IN);
    }

    /* Not yet finished writing; re-arm */
    return fdo_set_state(&client->fdo, CLIENT_STATE_RECV_CMD_RES, FDH_TYPE_SOCKET_OUT);
}

int client_wakeup(client_t *client) {
    uint64_t i;
    i = 1;
    return (write(client->fdo.event_in.fd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}

int client_set_db_name(client_t *client, char *db_name, size_t db_name_len) {
    buf_clear(&client->db_name);
    buf_append_str_len(&client->db_name, db_name, db_name_len);
    return MYSW_OK;
}

int client_set_hint(client_t *client, char *hint, size_t hint_len) {
    buf_clear(&client->hint);
    buf_append_str_len(&client->hint, hint, hint_len);
    return MYSW_OK;
}

/*

0000: 5300 0000 0a35 2e36 2e34 312d 3834 2e31 2d6c 6f67 0012 fc7c 0036 2d7c 4c45 5456  S....5.6.41-84.1-log...|.6-|LETV
0020: 3300 fff7 2102 007f 8015 0000 0000 0000 0000 0000 3c72 4d69 4767 4f38 6a2d 3d2d  3...!...............<rMiGgO8j-=-
0040: 006d 7973 716c 5f6e 6174 6976 655f 7061 7373 776f 7264 00                        .mysql_native_password.

0000: c200 0001 85a2 3f00 0000 0001 2100 0000 0000 0000 0000 0000 0000 0000 0000 0000  ......?.....!...................
0020: 0000 0000 XXXX XXXX 5f61 646d 696e 0014 10e6 9424 ebbb 6183 9255 e5f9 310e 332f  ....XXXX_admin.....$..a..U..1.3/
0040: 39ea 3f69 6d79 7371 6c5f 6e61 7469 7665 5f70 6173 7377 6f72 6400 6b03 5f6f 7305  9.?imysql_native_password.k._os.
0060: 4c69 6e75 780c 5f63 6c69 656e 745f 6e61 6d65 086c 6962 6d79 7371 6c04 5f70 6964  Linux._client_name.libmysql._pid
0080: 0531 3431 3431 0f5f 636c 6965 6e74 5f76 6572 7369 6f6e 0b35 2e36 2e34 312d 3834  .14141._client_version.5.6.41-84
00a0: 2e31 095f 706c 6174 666f 726d 0678 3836 5f36 340c 7072 6f67 7261 6d5f 6e61 6d65  .1._platform.x86_64.program_name
00c0: 056d 7973 716c                                                                   .mysql

0000: 0700 0002 0000 0002 0000 00 ...........

0000: 2100 0000 0373 656c 6563 7420 4040 7665 7273 696f 6e5f 636f 6d6d 656e 7420 6c69  !....select @@version_comment li
0020: 6d69 7420 31                                                                     mit 1

0000: 0100 0001 0127 0000 0203 6465 6600 0000 1140 4076 6572 7369 6f6e 5f63 6f6d 6d65  .....'....def....@@version_comme
0020: 6e74 000c 2100 a200 0000 fd00 001f 0000 0500 0003 fe00 0002 0037 0000 0436 5065  nt..!....................7...6Pe
0040: 7263 6f6e 6120 5365 7276 6572 2028 4750 4c29 2c20 5265 6c65 6173 6520 3834 2e31  rcona Server (GPL), Release 84.1
0060: 2e31 2c20 5265 7669 7369 6f6e 2062 3330 3836 3139 0500 0005 fe00 0002 00         .1, Revision b308619.........

0000: 0a00 0000 0373 656c 6563 7420 3432  .....select 42

0000: 0100 0001 0118 0000 0203 6465 6600 0000 0234 3200 0c3f 0002 0000 0008 8100 0000  ..........def....42..?..........
0020: 0005 0000 03fe 0000 0200 0300 0004 0234 3205 0000 05fe 0000 0200                 ...............42.........

0000: 0100 0000 01 .....

*/
