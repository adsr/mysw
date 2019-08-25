#include "mysw.h"

static int client_process_recv_handshake_init(client_t *client);
static int client_process_send_handshake_init_res(client_t *client);
static int client_process_recv_handshake_res(client_t *client);
static int client_process_send_cmd(client_t *client);
static int client_process_send_cmd_inner(client_t *client);
static int client_process_wait_cmd_res(client_t *client);
static int client_process_recv_cmd_res(client_t *client);
static int client_set_state(client_t *client, int state, fdh_t *fdh);


int client_new(proxy_t *proxy, int connfd, client_t **out_client) {
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
    /* TODO pthread_spin_destroy */

    /* Init socket event handle (network io) */
    fdh_init(&client->fdh_socket_in, &client->fdh_socket_out, proxy->fdpoll, client, &client->spinlock, FDH_TYPE_SOCKET, connfd, client_process);

    /* Init application event handle (internal eventfd) */
    fdh_init(&client->fdh_event, NULL, proxy->fdpoll, client, &client->spinlock, FDH_TYPE_EVENT, efd, client_process);

    /* Initially poll for writeable client socket */
    client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_INIT, &client->fdh_socket_out);

    if (out_client) *out_client = client;

    return MYSW_OK;
}

int client_process(fdh_t *fdh) {
    int rv;
    client_t *client;

    client = fdh->udata;

    if (client->fdh_socket_in.read_eof || client->fdh_socket_in.read_write_errno || client->fdh_socket_out.read_write_errno) {
        /* The client disconnected or there was a read/write error */
        /* TODO mark for destruction instead of destroying */
        /* TODO write async event that loops until refcount==0 to safely destroy the client */
        return client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_INIT, NULL);
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
    fdh_t *fdh_socket;

    fdh_socket = &client->fdh_socket_out;

    if (!fdh_is_writing(fdh_socket)) {
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
        fdh_reset_rw_state(fdh_socket);
        out = &fdh_socket->buf;
        buf_clear(out);
        buf_append_str_len(out, "\x00\x00\x00", 3);             /* payload len (3) (set below) */
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

    if (fdh_is_write_finished(fdh_socket)) {
        /* Finished writing; transition state */
        return client_set_state(client, CLIENT_STATE_SEND_HANDSHAKE_INIT_RES, &client->fdh_socket_in);
    }

    /* Not yet finished writing; re-arm */
    return client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_INIT, &client->fdh_socket_out);
}

/* The client finished receiving the handshake packet. We now expect the client
 * to send a response including auth. Then we reply with either an OK or an ERR
 * packet, and the connection moves into the command phase.
 */
static int client_process_send_handshake_init_res(client_t *client) {
    buf_t *in, *out;
    size_t cur;
    int capability_flags;
    int max_packet_size;
    int server_status_flags;
    int charset;
    uint8_t sequence_id;
    int payload_len;
    char *username, *auth, *dbname, *plugin;
    size_t username_len, auth_len, dbname_len, plugin_len;

    in = &client->fdh_socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        /* Have not received a complete packet yet; re-arm */
        return client_set_state(client, CLIENT_STATE_SEND_HANDSHAKE_INIT_RES, &client->fdh_socket_in);
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
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        auth_len = buf_get_u8(in, cur);                 cur += 1;
        auth = buf_get_str(in, cur);                    cur += auth_len;
    } else {
        /* TODO not clear what to expect here */
        auth = buf_get_str0(in, cur, &auth_len);        cur += auth_len;
    }
    if (capability_flags & MYSQLD_CLIENT_CONNECT_WITH_DB) {
        dbname = buf_get_str0(in, cur, &dbname_len);    cur += dbname_len + 1;
        /* TODO remember dbname */
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
    (void)dbname;
    (void)plugin;
    (void)charset;
    (void)max_packet_size;
    (void)payload_len;

    /* Queue OK packet */
    fdh_reset_rw_state(&client->fdh_socket_out);
    out = &client->fdh_socket_out.buf;
    server_status_flags = MYSQLD_SERVER_STATUS_AUTOCOMMIT; /* TODO figure out init server_status_flags */
    buf_clear(out);
    buf_append_str_len(out, "\x00\x00\x00", 3);             /* payload len (3) (set below) */
    buf_append_u8(out, sequence_id + 1);                    /* sequence id (1) */
    buf_append_u8(out, '\x00');                             /* OK packet header */
    buf_append_u8(out, '\x00');                             /* affected rows (lenenc) */
    buf_append_u8(out, '\x00');                             /* last insert id (lenenc) */
    buf_append_u16(out, server_status_flags);               /* server status flags (2) */
    buf_append_u16(out, 0);                                 /* warnings (2) */
    buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */

    /* Transition state */
    return client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_RES, &client->fdh_socket_out);
}

static int client_process_recv_handshake_res(client_t *client) {
    if (!fdh_is_write_finished(&client->fdh_socket_out)) {
        /* We have not finished writing to the client; re-arm */
        return client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_RES, &client->fdh_socket_out);
    }

    /* We have finished writing to the client; transition state */
    fdh_reset_rw_state(&client->fdh_socket_in);
    return client_set_state(client, CLIENT_STATE_SEND_CMD, &client->fdh_socket_in);
}


static int client_process_send_cmd(client_t *client) {
    buf_t *in;

    in = &client->fdh_socket_in.buf;

    if (!util_has_complete_mysql_packet(in)) {
        /* Client has not sent a complete packet yet; re-arm */
        return client_set_state(client, CLIENT_STATE_SEND_CMD, &client->fdh_socket_in);
    }

    cmd_init(client, in, &client->cmd);

    if (client->cmd.cmd_byte == MYSQLD_COM_QUIT) {
        /* TODO client destroy */
        return client_set_state(client, CLIENT_STATE_RECV_HANDSHAKE_INIT, NULL);
    }

    return client_process_send_cmd_inner(client);
}

static int client_process_send_cmd_inner(client_t *client) {
    if (client->in_txn || client->in_prep_stmt) {
        /* Need specific mysqld */
        /* TODO assert target_server is present*/
        /* TODO assert target_server is reserved for us */
        server_set_client(client->target_server, client);
        server_wakeup(client->target_server);
    } else if (!client->target_pool || client->cmd.is_target_cmd) {
        /* Need to target */
        targeter_queue_client(client->proxy->targeter, client);
        targeter_wakeup(client->proxy->targeter);
    } else {
        /* Use existing target (client->target_pool) */
        pool_queue_client(client->target_pool, client);
        pool_wakeup(client->target_pool);
    }

    return client_set_state(client, CLIENT_STATE_WAIT_CMD_RES, &client->fdh_event);
}

static int client_process_wait_cmd_res(client_t *client) {
    fdh_t *fdh_socket;
    buf_t *out;

    fdh_socket = &client->fdh_socket_out;
    out = &fdh_socket->buf;

    fdh_reset_rw_state(fdh_socket);
    buf_copy_from(out, &client->cmd_result);
    buf_clear(&client->cmd_result);

    return client_set_state(client, CLIENT_STATE_RECV_CMD_RES, &client->fdh_socket_out);

    /* TODO mult statements
    
    if (!client->cmd.stmt_cur->next) {
        |* No more stmts. Time to send results.*|
        return client_set_state(client, CLIENT_STATE_RECV_CMD_RES, &client->fdh_event, EPOLLOUT);
    }

    |* Process next stmt in cmd *|
    client->cmd.stmt_cur = client->cmd.stmt_cur->next;
    return client_process_send_cmd_inner(client);

    */
}

static int client_process_recv_cmd_res(client_t *client) {
    fdh_t *fdh_socket;

    fdh_socket = &client->fdh_socket_out;

    if (fdh_is_write_finished(fdh_socket)) {
        /* Finished writing; transition state */
        fdh_reset_rw_state(&client->fdh_socket_in);
        return client_set_state(client, CLIENT_STATE_SEND_CMD, &client->fdh_socket_in);
    }

    /* Not yet finished writing; re-arm */
    return client_set_state(client, CLIENT_STATE_RECV_CMD_RES, &client->fdh_socket_out);
}

int client_destroy(client_t *client) {
    fdh_ensure_unwatched(&client->fdh_socket_in);
    fdh_ensure_unwatched(&client->fdh_socket_out);
    fdh_ensure_unwatched(&client->fdh_event);
    close(client->fdh_socket_in.fd);
    close(client->fdh_socket_out.fd);
    close(client->fdh_event.fd);
    buf_free(&client->fdh_socket_in.buf);
    buf_free(&client->fdh_socket_out.buf);
    /* TODO avoid use after free, e.g., other objects may have client pointer */
    free(client);
    return MYSW_OK;
}

int client_wakeup(client_t *client) {
    uint64_t i;
    i = 1;
    return (write(client->fdh_event.fd, &i, sizeof(i)) == sizeof(i)) ? MYSW_OK : MYSW_ERR;
}

static int client_set_state(client_t *client, int state, fdh_t *fdh) {
    int rv;

    if (fdh != &client->fdh_socket_in)  fdh_ensure_unwatched(&client->fdh_socket_in);
    if (fdh != &client->fdh_socket_out) fdh_ensure_unwatched(&client->fdh_socket_out);
    if (fdh != &client->fdh_event)      fdh_ensure_unwatched(&client->fdh_event);

    /* Watch the specified fdh */
    if (fdh) try(rv, fdh_ensure_watched(fdh));

    /* Set state */
    client->state = state;
    return MYSW_OK;
}

/*

static int client_process_command_executing(client_t *client) {
    stmt_t *stmt;

    stmt = client->stmt_list;
    if (!stmt) {
        client->state = CLIENT_STATE_COMMAND_READY;
        client->fdh_socket.
        fdh_watch(&client->fdh_socket); // TODO handle connection error (retry later?)
    }
    while (stmt) {
        if (strncasecmp("use", stmt->cmd, stmt->cmd_len) == 0) {
            
        }
        stmt = stmt->next;
    }

    /* TODO parse out USE statement, retarget, forward */
    /* TODO prevent retarget in transaction */
    /* TODO foreach stmt:
     *        if use: set db
     *        ask user-script where to go (array for teeing, empty for blackhole)
     *        queue command on target
     *        target moves into reserved state (rwlock)
     *        wakeup target via eventfd write
     *        target wakes up and dequeues command
     *        
    /* TODO queue command on target pool or specific server (rwlock) */
    /* TODO wakeup pool or specific server (eventfd)


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
