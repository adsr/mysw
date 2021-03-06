#include "mysw.h"

// grep ^static client.c | sed 's| {|;|g'
static int client_handle();
static int client_handle_handshake_write(client_t *client);
static int client_handle_handshake_read_and_response(client_t *client);
static int client_handle_command(client_t *client);
static int client_filter_response(client_t *client);
static int client_acquire_server(client_t *client);
static int client_release_server(client_t *client);
static int client_yield_write_response(client_t *client);
static int client_yield_write_buf(client_t *client);
static int client_yield_read_event(client_t *client);
static int client_yield_read_packet(client_t *client);
static int client_yield_write_ok(client_t *client);
static int client_yield_write_err(client_t *client, uint16_t errcode, char *errmsg);
static int client_deinit(client_t *client);

static int client_handle() {
    int rv;
    client_t *client;

    client = (client_t*)aco_get_arg();

    if_err_goto(rv, client_handle_handshake_write(client), client_handle_finish);
    if_err_goto(rv, client_handle_handshake_read_and_response(client), client_handle_finish);
    while (!mysw.done && client->alive) {
        if_err_break(rv, client_handle_command(client));
    }

client_handle_finish:
    if (client->needs_response) {
        if (rv == MYSW_OK) {
            if_err_goto(rv, client_yield_write_ok(client), client_handle_exit);
        } else if (rv == MYSW_ERR) {
            if_err_goto(rv, client_yield_write_err(client, 9000, "Server error"), client_handle_exit);
        }
    }

client_handle_exit:
    client_deinit(client);

    aco_exit();
}

static int client_handle_handshake_write(client_t *client) {
    int i;

    // init challenge
    uint8_t challenge[21];
    for (i = 0; i < 20; ++i) challenge[i] = (uint8_t)rand();
    challenge[20] = '\x00';

    // init caps
    uint32_t capability_flags;
    capability_flags = MYSQLD_CLIENT_PLUGIN_AUTH \
        | MYSQLD_CLIENT_SECURE_CONNECTION \
        | MYSQLD_CLIENT_CONNECT_WITH_DB \
        | MYSQLD_CLIENT_PROTOCOL_41;

    // write handshake packet
    // TODO support other caps, auth, ssl
    buf_t *buf;
    buf = &client->wbuf;
    buf_clear(buf);
    buf_append_u24(buf, 0);                                 // payload len (3) (set below)
    buf_append_u8(buf, '\x00');                             // sequence id (1)
    buf_append_u8(buf, '\x0a');                             // protocol version
    buf_append_str_len(buf, "mysw\x00", 5);                 // TODO server version
    buf_append_str_len(buf, "\x00\x00\x00\x00", 4);         // TODO connection id
    buf_append_str_len(buf, (char *)challenge, 8);          // challenge[:8]
    buf_append_u8(buf, '\x00');                             // filler
    buf_append_u16(buf, capability_flags & 0xffff);         // cap flags lower 2
    buf_append_u8(buf, '\x21');                             // TODO character set
    buf_append_u16(buf, MYSQLD_SERVER_STATUS_AUTOCOMMIT);   // TODO status flags
    buf_append_u16(buf, (capability_flags >> 16) & 0xffff); // cap flags upper 2
    buf_append_u8(buf, 21);                                 // len(challenge)
    buf_append_u8_repeat(buf, '\x00', 10);                  // filler
    buf_append_str_len(buf, (char *)(challenge + 8), 13);   // challenge[8:]
    buf_append_str_len(buf, "mysql_native_password\x00", 22);
    buf_set_u24(buf, 0, buf_len(buf) - 4);                  // set payload len
    return client_yield_write_buf(client);
}

static int client_handle_handshake_read_and_response(client_t *client) {
    int rv;
    buf_t *buf;
    size_t cursor, username_len, auth_len, db_name_len, plugin_len;
    uint32_t payload_len, capability_flags, max_packet_size;
    uint8_t sequence_id, charset;
    char *username, *auth, *db_name, *plugin;

    buf = &client->rbuf;

    // read handshake response packet
    buf_clear(buf);
    if_err_return(rv, client_yield_read_packet(client));

    // parse handshake response packet
    cursor = 0;
    payload_len = buf_get_u24(buf, cursor);                 cursor += 3;
    sequence_id = buf_get_u8(buf, cursor);                  cursor += 1;
    capability_flags = buf_get_u32(buf, cursor);            cursor += 4;
    max_packet_size = buf_get_u32(buf, cursor);             cursor += 4;
    charset = buf_get_u8(buf, cursor);                      cursor += 1;
    cursor += 23; // filler
    username = buf_get_str0(buf, cursor, &username_len);    cursor += username_len + 1;
    buf_assign_str_len(&client->username, username, username_len);
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        auth_len = buf_get_u8(buf, cursor);                 cursor += 1;
        auth = buf_get_str(buf, cursor);                    cursor += auth_len;
    } else {
        // TODO not clear what to expect here
        auth = buf_get_str0(buf, cursor, &auth_len);        cursor += auth_len;
    }
    if (capability_flags & MYSQLD_CLIENT_CONNECT_WITH_DB) {
        db_name = buf_get_str0(buf, cursor, &db_name_len);  cursor += db_name_len + 1;
        buf_assign_str_len(&client->db_name, db_name, db_name_len);
    }
    if (capability_flags & MYSQLD_CLIENT_PLUGIN_AUTH) {
        plugin = buf_get_str0(buf, cursor, &plugin_len);    cursor += plugin_len + 1;
    }

    if (strncmp(plugin, "mysql_native_password", plugin_len) != 0) {
        if_err_return(rv, client_yield_write_err(client, 9000, "Expected mysql_native_password auth"));
        return MYSW_ERR;
    }

    // TODO actually auth
    (void)username;
    (void)auth;
    (void)db_name;
    (void)plugin;
    (void)charset;
    (void)max_packet_size;
    (void)payload_len;

    // write ok
    client->status_flags = MYSQLD_SERVER_STATUS_AUTOCOMMIT;
    client->last_sequence_id = sequence_id;
    if_err_return(rv, client_yield_write_ok(client));

    return MYSW_OK;
}

static int client_handle_command(client_t *client) {
    int rv;
    buf_t *rbuf;
    uint8_t cmd_byte;

    rv = 0;
    rbuf = &client->rbuf;

    // read command
    buf_clear(rbuf);
    if_err_return(rv, client_yield_read_packet(client));

    // get cmd byte
    cmd_byte = buf_get_u8(rbuf, 4);

    // response
    switch (cmd_byte) {
        default:
            client->needs_response = 1;
            // fallthrough
        case MYSQLD_COM_STMT_SEND_LONG_DATA:
        case MYSQLD_COM_STMT_CLOSE:
            if (!client->target_server) {
                if_err_break(rv, client_acquire_server(client));
            }
            if_err_break(rv, server_wakeup(client->target_server));
            if (client->needs_response) {
client_handle_command_read_write:
                if_err_break(rv, client_yield_read_event(client));
                if_err_break(rv, client_filter_response(client));
                if_err_break(rv, client_yield_write_response(client));
                if (client->has_more_results) {
                    goto client_handle_command_read_write;
                }
                client->has_more_results = 0;
                client->needs_response = 0;
            }
            if (!client->in_txn && client->prep_stmt_count == 0) {
                if_err_break(rv, client_release_server(client));
            }
            break;
        case MYSQLD_COM_INIT_DB:
            if_err_break(rv, client_release_server(client));
            buf_clear(&client->target_pool);
            buf_assign_str_len(&client->db_name, (char*)(rbuf->data + 5), rbuf->len - 5);
            if_err_break(rv, client_yield_write_ok(client));
            break;
        case MYSQLD_COM_QUIT:
            client->alive = 0;
            break;
    }

    if (rv != MYSW_OK) {
        client->alive = 0;
    }

    return rv;
}

static int client_filter_response(client_t *client) {
    return MYSW_ERR;
}

static int client_acquire_server(client_t *client) {
    int rv;
    if (buf_empty(&client->target_pool)) {
        // TODO pass leading comment to targeter?
        if_err_return(rv, targeter_queue_request(client));
        if_err_return(rv, client_yield_read_event(client));
        if (buf_empty(&client->target_pool)) {
            if_err_return(rv, client_yield_write_err(client, 9000, "No target pool"));
            return MYSW_ERR;
        }
    }
    // TODO reserve a server in target_pool
    // TODO keep trying until timeout
    if (!client->target_server) {
        if_err_return(rv, client_yield_write_err(client, 9000, "No slots in target pool"));
        return MYSW_ERR;
    }
    return MYSW_OK;
}

static int client_release_server(client_t *client) {
    return MYSW_ERR;
}

static int client_yield_write_response(client_t *client) {
    // read packet for in_txn, more results
}

static int client_yield_write_buf(client_t *client) {
    int rv;
    buf_t *buf;
    size_t cursor;
    ssize_t iorv;
    struct epoll_event event;

    buf = &client->wbuf;
    cursor = 0;

    while (cursor < buf->len) {
        iorv = write(client->fdh_socket.fd, buf->data + cursor, buf->len - cursor);
        if (iorv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // tx buffer is full (normally should not happen)
                // add socket to epoll with EPOLLOUT | EPOLLONESHOT
                event.events = EPOLLOUT | EPOLLONESHOT;
                event.data.ptr = &client->fdh_socket;
                rv = epoll_add_or_mod(client->worker->epollfd, client->fdh_socket.fd, &event);
                if (rv != 0) {
                    perror("client_yield_write_buf: epoll_add_or_mod");
                    return MYSW_ERR;
                }
                aco_yield();
            } else if (errno == EINTR) {
                // signal interrupted call (should not happen)
                continue;
            } else if (errno == EPIPE) {
                // client hung up
                return MYSW_EOF;
            } else {
                perror("client_yield_write_buf: write");
                return MYSW_ERR;
            }
        } else {
            cursor += iorv;
        }
    }

    return MYSW_OK;
}

static int client_yield_read_event(client_t *client) {
    ssize_t iorv;
    uint64_t u64;

    while (1) {
        iorv = read(client->fdh_event.fd, &u64, sizeof(u64));
        if (iorv > 0) {
            // data
            break;
        } else if (iorv == 0) {
            return MYSW_ERR; // TODO can this happen?
        } else {
            // error
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                aco_yield();
            } else if (errno == EINTR) {
                continue;
            } else {
                perror("client_yield_read_event: read");
                return MYSW_ERR;
            }
        }
    }

    return MYSW_OK;
}

static int client_yield_read_packet(client_t *client) {
    buf_t *buf;
    size_t read_len;
    ssize_t iorv;
    uint8_t read_chunk[256]; // TODO adjustable read size
    int is_reading_header;

    buf = &client->rbuf;
    is_reading_header = 1;
    read_len = 4; // header size

    while (buf->len < read_len) {
        iorv = read(client->fdh_socket.fd, read_chunk, sizeof(read_chunk));
        if (iorv > 0) {
            // data
            buf_append_str_len(buf, (char *)read_chunk, iorv);
            if (is_reading_header && buf->len >= 4) {
                // add payload len to read_len
                is_reading_header = 0;
                read_len += buf_get_u24(buf, 0);
            }
        } else if (iorv == 0) {
            // client hung up
            return MYSW_EOF;
        } else {
            // error
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                aco_yield();
            } else if (errno == EINTR) {
                continue;
            } else {
                perror("client_yield_read_packet: read");
                return MYSW_ERR;
            }
        }
    }

    return MYSW_OK;
}

static int client_yield_write_ok(client_t *client) {
    buf_t *buf;

    buf = &client->wbuf;
    buf_clear(buf);
    buf_append_u24(buf, 0);                           // payload len (3) (set below)
    buf_append_u8(buf, client->last_sequence_id + 1); // sequence id (1)
    buf_append_u8(buf, '\x00');                       // OK packet header (1)
    buf_append_u8(buf, '\x00');                       // affected rows (lenenc)
    buf_append_u8(buf, '\x00');                       // last insert id (lenenc)
    buf_append_u16(buf, client->status_flags);        // server status flags (2)
    buf_append_u16(buf, 0);                           // warnings (2)
    buf_set_u24(buf, 0, buf_len(buf) - 4);            // set payload len

    return client_yield_write_buf(client);
}

static int client_yield_write_err(client_t *client, uint16_t errcode, char *errmsg) {
    buf_t *buf;

    buf = &client->wbuf;
    buf_clear(buf);
    buf_append_u24(buf, 0);                           // payload len (3) (set below)
    buf_append_u8(buf, client->last_sequence_id + 1); // sequence id (1)
    buf_append_u8(buf, '\xff');                       // ERR packet header (1)
    buf_append_u16(buf, errcode);                     // err code (2)
    buf_append_u8(buf, '\x00');                       // sql_state_marker (1)
    buf_append_str_len(buf, "HY900", 5);              // sql_state (5)
    buf_append_str(buf, errmsg);                      // err msg (streof)
    buf_set_u24(buf, 0, buf_len(buf) - 4);            // set payload len

    return client_yield_write_buf(client);
}

static int client_deinit(client_t *client) {
    struct itimerspec its;
    uint64_t u64;

    // release target if needed
    if (client->target_server) {
        client_release_server(client);
    }

    // close socket if needed
    if (client->fdh_socket.fd >= 0) {
        epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_socket.fd, NULL);
        close(client->fdh_socket.fd);
        client->fdh_socket.fd = -1;
    }

    // disarm timerfd
    memset(&its, 0, sizeof(its));
    timerfd_settime(client->fdh_timer.fd, 0, &its, NULL);

    // drain eventfd (should return EAGAIN if no events)
    read(client->fdh_event.fd, &u64, sizeof(u64));

    // reset scalars
    client->alive = 0;
    client->last_sequence_id = 0;
    client->status_flags = 0;

    // reset buffers
    buf_clear(&client->wbuf);
    buf_clear(&client->rbuf);
    buf_clear(&client->username);
    buf_clear(&client->db_name);

    return MYSW_OK;
}

int client_wakeup(client_t *client) {
    uint64_t u64;
    u64 = 1;
    if (write(client->fdh_event.fd, &u64, sizeof(u64)) != sizeof(u64)) {
        return MYSW_ERR;
    }
    return MYSW_OK;
}

int client_create_all() {
    int rv, i;
    client_t *client;
    struct epoll_event event;

    // allocate clients
    mysw.clients = calloc(mysw.opt_max_num_clients, sizeof(client_t));
    mysw.clients_unused = mysw.clients;
    if ((rv = pthread_spin_init(&mysw.clients_lock.spinlock, PTHREAD_PROCESS_SHARED)) != 0) {
        errno = rv;
        perror("client_create_all: pthread_spin_init");
        return MYSW_ERR;
    }
    mysw.clients_lock.created = 1;

    // init fds to -1
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;
        client->fdh_socket.fd = -1;
        client->fdh_event.fd = -1;
        client->fdh_timer.fd = -1;
    }

    // init each client
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;
        client->num = i;

        // init next_unused
        if (i + 1 < mysw.opt_max_num_clients) {
            client->next_unused = mysw.clients + i + 1;
        }

        // init fdo
        client->fdo.co_func = client_handle;
        client->fdo.co_arg = client;
        client->fdo.alive = &client->alive;
        client->fdh_socket.fdo = &client->fdo;
        client->fdh_event.fdo = &client->fdo;
        client->fdh_timer.fdo = &client->fdo;

        // assign worker
        client->worker = mysw.workers + (client->num % mysw.opt_num_workers);

        // create eventfd
        if ((client->fdh_event.fd = eventfd(0, EFD_NONBLOCK)) < 0) {
            perror("client_create_all: eventfd");
            return MYSW_ERR;
        }

        // create timerfd
        if ((client->fdh_timer.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
            perror("client_create_all: timerfd_create");
            return MYSW_ERR;
        }

        // add eventfd to worker epoll
        event.events = EPOLLIN;
        event.data.ptr = &client->fdh_event;
        if (epoll_ctl(client->worker->epollfd, EPOLL_CTL_ADD, client->fdh_event.fd, &event) < 0) {
            perror("client_create_all: epoll_ctl");
            return MYSW_ERR;
        }

        // add timertfd to worker epoll
        event.events = EPOLLIN;
        event.data.ptr = &client->fdh_timer;
        if (epoll_ctl(client->worker->epollfd, EPOLL_CTL_ADD, client->fdh_timer.fd, &event) < 0) {
            perror("client_create_all: epoll_ctl");
            return MYSW_ERR;
        }
    }

    return MYSW_OK;
}

int client_free_all() {
    int i;
    client_t *client;
    fdo_t *fdo;

    // bail if not allocated
    if (!mysw.clients) {
        return MYSW_OK;
    }

    // free lock
    if (mysw.clients_lock.created) {
        pthread_spin_destroy(&mysw.clients_lock.spinlock);
    }

    // free each client
    for (i = 0; i < mysw.opt_max_num_clients; ++i) {
        client = mysw.clients + i;

        // deinit (closes socket)
        client_deinit(client);

        // close eventfd
        if (client->fdh_event.fd >= 0) {
            epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_event.fd, NULL);
            close(client->fdh_event.fd);
        }

        // close timerfd
        if (client->fdh_timer.fd >= 0) {
            epoll_ctl(client->worker->epollfd, EPOLL_CTL_DEL, client->fdh_timer.fd, NULL);
            close(client->fdh_timer.fd);
        }

        // free coroutine resources
        fdo = &client->fdo;
        if (fdo->co) aco_destroy(fdo->co);
        if (fdo->co_stack) aco_share_stack_destroy(fdo->co_stack);
    }

    free(mysw.clients);

    return MYSW_OK;
}
