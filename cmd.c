#include "mysw.h"

int cmd_init(client_t *client, buf_t *in, cmd_t *cmd) {
    memset(cmd, 0, sizeof(cmd_t));
    cmd->client = client;
    cmd->cmd_byte = buf_get_u8(in, 4);
    cmd->payload = in;
    if (cmd->cmd_byte == MYSQLD_COM_QUERY || cmd->cmd_byte == MYSQLD_COM_STMT_PREPARE) {
        cmd_lex_sql(in, cmd);
    }
    return MYSW_OK;
}

int cmd_lex_sql(buf_t *in, cmd_t *cmd) {
    size_t i;
    char b, c, d;
    int in_slash_comment, in_dash_comment, in_hash_comment;
    int in_double_quote, in_single_quote, in_backtick;
    int in_word;
    size_t ss, ts;

    cmd->sql = buf_get_streof(in, 5, &cmd->sql_len);

    in_slash_comment = 0;
    in_dash_comment = 0;
    in_hash_comment = 0;
    in_double_quote = 0;
    in_single_quote = 0;
    in_backtick = 0;
    in_word = 0;

    ss = 0;
    ts = 0;
    b = '\0';
    for (i = 0; i < cmd->sql_len; ++i) {
        c = cmd->sql[i];
        d = (i < cmd->sql_len - 1) ? cmd->sql[i + 1] : '\0';
        if (in_slash_comment) {
            if (c == '*' && d == '/') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_COMMENT, &ts, i + 1);
                in_slash_comment = 0;
                ++i;
            }
        } else if (in_dash_comment) {
            if (c == '\n') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_COMMENT, &ts, i);
                in_dash_comment = 0;
            }
        } else if (in_hash_comment) {
            if (c == '\n') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_COMMENT, &ts, i);
                in_hash_comment = 0;
            }
        } else if ((in_double_quote || in_single_quote) && c == '\\') {
            ++i;
        } else if (in_double_quote && c == '"' && d == '"') {
            ++i;
        } else if (in_single_quote && c == '\'' && d == '\'') {
            ++i;
        } else if (in_double_quote) {
            if (c == '"') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_STRING, &ts, i - 1);
                in_double_quote = 0;
            }
        } else if (in_single_quote) {
            if (c == '\'') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_STRING, &ts, i - 1);
                in_single_quote = 0;
            }
        } else if (in_backtick) {
            if (c == '`') {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_BACKTICK, &ts, i - 1);
                in_backtick = 0;
            }
        } else {
            if (in_word) {
                /* Permitted characters in unquoted identifiers:
                 *
                 *   ASCII: [0-9,a-z,A-Z$_] (basic Latin letters, digits 0-9, dollar, underscore)
                 *   Extended: U+0080 .. U+FFFF
                 *
                 * TODO not handling wide chars for now
                 *
                 * https://dev.mysql.com/doc/refman/8.0/en/identifiers.html */
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$')) {
                    cmd_lex_sql_emit_token(cmd, STMT_TOKEN_WORD, &ts, i);
                    in_word = 0;
                }
            }
            if (c == ';') {
                /* If cmd->cmd_byte is MYSQLD_COM_STMT_PREPARE, this is going
                 * to be a syntax error. */
                cmd_lex_sql_end_stmt(cmd, &ss, i);
            } else if (c == '`') {
                in_backtick = 1;
                ts = i + 1;
            } else if (c == '\'') {
                in_single_quote = 1;
                ts = i + 1;
            } else if (c == '"') {
                in_double_quote = 1;
                ts = i + 1;
            } else if ((b == '\n' || b == '\0') && c == '#') {
                in_hash_comment = 1;
                ts = i;
            } else if ((b == '\n' || b == '\0') && c == '-' && d == '-') {
                in_dash_comment = 1;
                ts = i;
                ++i;
            } else if (c == '/' && d == '*') {
                in_slash_comment = 1;
                ts = i;
                ++i;
            } else if (!in_word && !isspace(c)) {
                in_word = 1;
                ts = i;
            }
        }
        b = c;
    }

    cmd_lex_sql_emit_token(cmd, STMT_TOKEN_WORD, &ts, i);
    cmd_lex_sql_end_stmt(cmd, &ss, i);
    cmd->stmt_cur = cmd->stmt_list;

    return MYSW_ERR;
}

int cmd_lex_sql_emit_token(cmd_t *cmd, int type, size_t *start, size_t end) {
    stmt_t *stmt;

    stmt = cmd->stmt_parsing;

    if (!stmt) {
        stmt = calloc(1, sizeof(stmt_t));
        LL_APPEND(cmd->stmt_list, stmt);
        cmd->stmt_parsing = stmt;
    }

    if (end > *start) {
        if (type == STMT_TOKEN_COMMENT) {
            if (stmt->comment == NULL && stmt->first == NULL) {
                stmt->comment = cmd->sql + *start;
                stmt->comment_len = end - *start;
            }
        } else if (stmt->first == NULL) {
            stmt->first = cmd->sql + *start;
            stmt->first_len = end - *start;
        } else if (stmt->second == NULL) {
            stmt->second = cmd->sql + *start;
            stmt->second_len = end - *start;
        }
    }

    *start = end;
    return MYSW_OK;
}

int cmd_lex_sql_end_stmt(cmd_t *cmd, size_t *start, size_t end) {
    stmt_t *stmt;

    stmt = cmd->stmt_parsing;

    if (stmt) {
        stmt->sql = cmd->sql + *start;
        stmt->sql_len = (end > *start) ? (end - *start) : 0;
    }

    cmd->stmt_parsing = NULL;

    *start = end;
    return MYSW_OK;
}

int cmd_is_targeting(cmd_t *cmd) {
    if (cmd->cmd_byte == MYSQLD_COM_INIT_DB) {
        /* COM_INIT_DB */
        return 1;
    } else if (cmd->cmd_byte == MYSQLD_COM_QUERY
        && cmd->stmt_cur
        && strncasecmp(cmd->stmt_cur->first, "USE", cmd->stmt_cur->first_len) == 0
    ) {
        /* USE statement */
        return 1;
    }
    return 0;
}

int cmd_is_use_stmt(cmd_t *cmd) {
}

int cmd_deinit(cmd_t *cmd) {
    stmt_t *stmt, *stmt_tmp;
    LL_FOREACH_SAFE(cmd->stmt_list, stmt, stmt_tmp) {
        LL_DELETE(cmd->stmt_list, stmt);
        free(stmt);
    }
    return MYSW_ERR;
}

/* 

int cmd_process_target(cmd_t *cmd) {
    client_t *client;

    client = cmd->client;

    |* TODO ERR packet *|
    |* TODO return client_set_state(client, CLIENT_STATE_RECV_ERR, &client->fdh_socket, EPOLLOUT); *|
    |* TODO return client_set_state(client, CLIENT_STATE_WAIT_CMD_RES, &client->fdh_event, EPOLLIN); *|

    if (cmd->cmd_byte == MYSQLD_COM_QUERY) {
        |*
        look at cur stmt
        if no stmt left, wakeup client
        elif use stmt, set dbname and curl for pool name (set_state)
        elif pool, add stmt to pool.queue (set_state)
        elif no pool, err
        *|
    } else if (cmd->cmd_byte == MYSQLD_COM_INIT_DB) {
        |*
        set dbname and curl for pool name (set_state)
        *|
    } else if (cmd->cmd_byte == MYSQLD_COM_QUIT) {
        |*
        client destroy
        *|
    } else nif (!cmd->target_pool) {
        |* error packet *|
    } else {
        |* return cmd_process_queue()  *|
    }
            |* TODO quit *|
            break;
    }

    switch (cmd->cmd_byte) {
        case MYSQLD_COM_QUERY:
            if (!client->target_pool && cmd_target_via_use_stmt(cmd) != MYSW_OK) { |* TODO cmd_target_via* can block... need state machine *|
                |* TODO err *|
            }
            
        case MYSQLD_COM_STMT_PREPARE:
            |* TODO no multi statements in prepared statements *|
            sql = buf_get_streof(in, 5, &sql_len);
            printf("sql='%.*s'\n", (int)sql_len, sql);

            util_sql_lex(sql, sql_len, command_byte, &client->stmt_list);

            client->state = CLIENT_STATE_PROCESSING_stmt_list;
            client_handle_command_executing(client);
            break;
        case MYSQLD_COM_INIT_DB:
            cmd_target_via_init_db(cmd);
            break;
        case MYSQLD_COM_QUIT:
            |* TODO quit *|
            break;
        case MYSQLD_COM_STMT_PREPARE:

        default:
            |* TODO if we have a target, forward. if not, err *|
            break;
        |*
        COM_BINLOG_DUMP
        COM_BINLOG_DUMP_GTID
        COM_CHANGE_USER         TODO investigate
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
        COM_RESET_CONNECTION    TODO investigate
        COM_SET_OPTION          TODO investigate
        COM_SHUTDOWN
        COM_SLEEP
        COM_STATISTICS
        COM_STMT_CLOSE    COM_STMT_* needs a server-specific target
        COM_STMT_EXECUTE
        COM_STMT_FETCH
        COM_STMT_PREPARE
        COM_STMT_RESET
        COM_STMT_SEND_LONG_DATA
        COM_TABLE_DUMP
        COM_TIME
        *|
    }


    |* TODO some commands expect a response, others do not *|
    |*      set epoll_flags depending on that *|
    |* commands with no response:
        COM_STMT_CLOSE
        COM_STMT_SEND_LONG_DATA
        COM_QUIT (close conn)
    *|
}

*/