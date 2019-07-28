#include "mysw.h"

static int client_process_command_ready(client_t *client);
static int client_process_connected(client_t *client);
static int client_process_handshake_received_init(client_t *client);
static int client_process_handshake_sent_response(client_t *client);
static int client_process_command_ready(client_t *client);
static int client_process_command_awaiting_response(client_t *client);
static int client_destroy(client_t *client);

int client_new(worker_t *worker, int connfd, client_t **out_client) {
    client_t *client;
    client = calloc(1, sizeof(client_t));

    client->fdh_conn.proxy = worker->proxy;
    client->fdh_conn.type = FDH_TYPE_CLIENT;
    client->fdh_conn.u.client = client;
    client->fdh_conn.fd = connfd;
    client->fdh_conn.epoll_flags = EPOLLOUT;

    client->fdh_event.proxy = worker->proxy;
    client->fdh_event.type = FDH_TYPE_CLIENT;
    client->fdh_event.u.client = client;
    client->fdh_event.fd = eventfd(0, EFD_NONBLOCK);
    client->fdh_event.epoll_flags = EPOLLIN;

    client->state = CLIENT_STATE_CONNECTED;

    fdh_watch(&client->fdh_conn);

    if (out_client) *out_client = client;

    return client_process(client);
}

int client_process(client_t *client) {
    if (client->fdh_conn.eof) {
        /* TODO client disconnected */
        client_destroy(client);
    } else if (client->fdh_conn.last_errno) {
        /* TODO there was a read/write error */
        client_destroy(client);
    }
    switch (client->state) {
        case CLIENT_STATE_CONNECTED:
            client_process_connected(client);
            break;
        case CLIENT_STATE_HANDSHAKE_RECEIVED_INIT:
            client_process_handshake_received_init(client);
            break;
        case CLIENT_STATE_HANDSHAKE_SENT_RESPONSE:
            client_process_handshake_sent_response(client);
            break;
        case CLIENT_STATE_COMMAND_READY:
            client_process_command_ready(client);
            break;
        case CLIENT_STATE_COMMAND_AWAITING_RESPONSE:
            client_process_command_awaiting_response(client);
            break;
        default:
            fprintf(stderr, "client_process: Invalid client state %d\n", client->state);
            break;
    }
    return 0;
}

/**
 * The client just connected. Send an Initial Handshake Packet.
 * https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase.html
 */
static int client_process_connected(client_t *client) {
    int capability_flags, i;
    char challenge[21];
    buf_t *out;
    fdh_t *fdh_conn;

    fdh_conn = &client->fdh_conn;

    if (fdh_is_write_finished(fdh_conn)) {
        /* Finished writing handshake packet. Transition state. */
        fdh_reset_rw_state(fdh_conn);
        client->state = CLIENT_STATE_HANDSHAKE_RECEIVED_INIT;
        client->fdh_conn.epoll_flags = EPOLLIN; /* poll for readable client */
    } else if (fdh_is_writing(fdh_conn)) {
        /* Continue writing packet (do nothing) */
    } else {
        /* Send handshake packet */

        /* Init challenge */
        for (i = 0; i < 20; ++i) challenge[i] = rand();
        challenge[20] = '\x00';

        /* Init caps */
        capability_flags = MYSQLD_CLIENT_PLUGIN_AUTH \
            | MYSQLD_CLIENT_SECURE_CONNECTION \
            | MYSQLD_CLIENT_CONNECT_WITH_DB \
            | MYSQLD_CLIENT_PROTOCOL_41;

        /* Build init packet */
        /* TODO support other caps, auth, ssl */
        out = &fdh_conn->out;
        buf_clear(out);
        buf_append_str_len(out, "\x00\x00\x00", 3);             /* payload len (3) (set below) */
        buf_append_u8(out, '\x00');                             /* sequence id (1) */
        buf_append_u8(out, '\x0a');                             /* protocol version */
        buf_append_str_len(out, "derp\x00", 5);                 /* TODO server version */
        buf_append_str_len(out, "\x00\x00\x00\x00", 4);         /* TODO connection id */
        buf_append_str_len(out, challenge, 8);                  /* challenge[:8] */
        buf_append_u8(out, '\x00');                             /* filler */
        buf_append_u16(out, capability_flags & 0xffff);         /* cap flags lower */
        buf_append_u8(out, '\x21');                             /* TODO character set */
        buf_append_u16(out, 0x0200);                            /* TODO status flags */
        buf_append_u16(out, (capability_flags >> 2) & 0xffff);  /* cap flags upper */
        buf_append_u8(out, 21);                                 /* len(challenge) */
        buf_append_str_len(out, "\x00\x00\x00\x00\x00\x00\x00\x00*x00\x00", 10);
        buf_append_str_len(out, challenge + 8, 13);             /* challenge[8:] */
        buf_append_str_len(out, "mysql_native_password\x00", 22);
        buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */

        client->fdh_conn.epoll_flags = EPOLLOUT; /* poll for writeable client */
    }
    return 0;
}

/**
 * The client finished receiving the handshake packet. We now expect the client
 * to send a response including auth. Then we reply with either an OK or an ERR
 * packet, and the connection moves into the command phase.
 */
static int client_process_handshake_received_init(client_t *client) {
    buf_t *in, *out;
    size_t cur;
    int capability_flags;
    int max_packet_size;
    int server_status_flags;
    int charset;
    char *username, *auth, *dbname, *plugin;
    size_t username_len, auth_len, dbname_len, plugin_len;

    in = &client->fdh_conn.in;
    if (!util_has_complete_mysql_packet(&client->fdh_conn.in)) {
        return 0;
    }

    /* https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_packets_protocol_handshake_response.html */
    cur = 0;
    capability_flags = buf_get_u32(in, cur);            cur += 4;
    max_packet_size = buf_get_u32(in, cur);             cur += 4;
    charset = buf_get_u8(in, cur);                      cur += 1;
    cur += 23; /* filler */
    username = buf_get_str0(in, cur, &username_len);    cur += username_len + 1;
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        auth_len = buf_get_u8(in, cur);                 cur += 1;
        auth = buf_get_str(in, cur);                    cur += auth_len;
    } else {
        auth_len = buf_get_u8(in, cur);                 cur += 1;
        auth_len = buf_get_u8(in, cur);                 cur += 1;
        auth = buf_get_str(in, cur);                    cur += auth_len;
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
        return 1;
    }

    /* TODO actually auth */
    (void)username;
    (void)auth;
    (void)dbname;
    (void)plugin;
    (void)charset;
    (void)max_packet_size;

    out = &client->fdh_conn.out;
    server_status_flags = MYSQLD_SERVER_STATUS_AUTOCOMMIT; /* TODO hmm */
    buf_clear(out);
    buf_append_str_len(out, "\x00\x00\x00", 3);             /* payload len (3) (set below) */
    buf_append_u8(out, '\x00');                             /* sequence id (1) */
    buf_append_u8(out, '\x00');                             /* OK packet header */
    buf_append_u8(out, '\x00');                             /* affected rows (lenenc) */
    buf_append_u8(out, '\x00');                             /* last insert id (lenenc) */
    buf_append_u16(out, server_status_flags);               /* server status flags (2) */
    buf_append_u16(out, 0);                                 /* warnings (2) */
    buf_set_u24(out, 0, buf_len(out) - 4);                  /* set payload len */

    client->state = CLIENT_STATE_HANDSHAKE_SENT_RESPONSE;
    client->fdh_conn.epoll_flags = EPOLLOUT; /* poll for writeable client */
    return 0;
}

static int client_process_handshake_sent_response(client_t *client) {
    if (fdh_is_write_finished(&client->fdh_conn)) {
        fdh_reset_rw_state(&client->fdh_conn);
        client->state = CLIENT_STATE_COMMAND_READY;
        client->fdh_conn.epoll_flags = EPOLLIN; /* poll for readable client */
    }
    return 0;
}

static int client_process_command_ready(client_t *client) {
    buf_t *in;
    int command_byte;
    char *sql;
    size_t sql_len;

    in = &client->fdh_conn.in;
    if (!util_has_complete_mysql_packet(&client->fdh_conn.in)) {
        return 0;
    }

    command_byte = buf_get_u8(in, 4);

    switch (command_byte) {
        case MYSQLD_COM_QUERY:
        case MYSQLD_COM_STMT_PREPARE:
            /* TODO parse out USE statement, retarget, forward */
            sql = buf_get_streof(in, 5, &sql_len);
        case MYSQLD_COM_INIT_DB:
            /* TODO retarget, forward */
            break;
        /* TODO? case COM_CHANGE_USER */
        /* TODO? case COM_RESET_CONNECTION */
        /* TODO? case COM_SET_OPTION */
        case MYSQLD_COM_QUIT:
            /* TODO quit */
            break;
        default:
            /* TODO if we have a target, forward. if not, err */
            break;
        /*
        COM_BINLOG_DUMP
        COM_BINLOG_DUMP_GTID
        COM_CHANGE_USER
        COM_CONNECT
        COM_CONNECT_OUT
        COM_CREATE_DB
        COM_DAEMON
        COM_DEBUG
        COM_DELAYED_INSERT
        COM_DROP_DB
        COM_FIELD_LIST
        COM_INIT_DB
        COM_PING
        COM_PROCESS_INFO
        COM_PROCESS_KILL
        COM_QUERY
        COM_QUIT
        COM_REFRESH
        COM_REGISTER_SLAVE
        COM_SET_OPTION
        COM_SHUTDOWN
        COM_SLEEP
        COM_STATISTICS
        COM_STMT_CLOSE
        COM_STMT_EXECUTE
        COM_STMT_FETCH
        COM_STMT_PREPARE
        COM_STMT_RESET
        COM_STMT_SEND_LONG_DATA
        COM_TABLE_DUMP
        COM_TIME
        */
    }

    (void)sql;

    /* TODO some commands expect a response, others do not */
    /*      set poll flags depending on that */

    /*
    timeline:
    - client/proxy do handshake
    - client sends "use ...; select ...;"
    - proxy parses sql
    - proxy processes "use...", sets client target (pool) (disallow retarget in transaction)
    - proxy processes "select...", adds command to pool queue
    - writes to pool's eventfd (or specific server eventfd if in txn)
    - worker picks up eventfd, calls pool_process / server_process
    - pool reserves a server to handle next command in pool queue
    - proxy writes command to server
    - server->processing_command = cmd
    - server responds
    - server writes response to client queue
    */

    return 0;
}

static int client_process_command_awaiting_response(client_t *client) {
    /* TODO await response */
    (void)client;
    return 0;
}

static int client_destroy(client_t *client) {
    close(client->fdh_conn.fd);
    buf_free(&client->fdh_conn.in);
    buf_free(&client->fdh_conn.out);
    close(client->fdh_event.fd);
    /* TODO eh */
    free(client);
    return 0;
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
