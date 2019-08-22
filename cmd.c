#include "mysw.h"

int cmd_init(client_t *client, buf_t *in, cmd_t *cmd) {
    memset(cmd, 0, sizeof(cmd_t));
    cmd->client = client;
    cmd->cmd_byte = buf_get_u8(in, 4);
    cmd->payload = in;
    if (cmd->cmd_byte == MYSQLD_COM_QUERY || cmd->cmd_byte == MYSQLD_COM_STMT_PREPARE) {
        cmd_parse_sql(cmd);
    }
    return MYSW_OK;
}

int cmd_parse_sql(cmd_t *cmd) {
    /* cmd->sql = buf_get_streof(in, 5, &cmd->sql_len); */
    /* TODO accept/expect parse into cmd->stmt_list */
    /* TODO no multi stmts for MYSQLD_COM_STMT_PREPARE */
    return MYSW_ERR;
}

int cmd_deinit(cmd_t *cmd) {
    /* TODO free cmd->stmt_list */
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

            util_sql_parse(sql, sql_len, command_byte, &client->stmt_list);

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