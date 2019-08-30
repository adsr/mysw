#include "mysw.h"

static int cmd_lex_sql_is_ident_char(char c);
static int cmd_lex_sql(buf_t *in, cmd_t *cmd);
static int cmd_lex_sql_emit_token(cmd_t *cmd, int type, size_t *start, size_t end);
static int cmd_lex_sql_end_stmt(cmd_t *cmd, size_t *start, size_t end);

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

static int cmd_lex_sql(buf_t *in, cmd_t *cmd) {
    size_t i;
    char b, c, d;
    int in_slash_comment, in_dash_comment, in_hash_comment;
    int in_double_quote, in_single_quote, in_backtick;
    int in_ident, in_non_ident;
    size_t ss, ts;
    int final_type;

    cmd->sql = buf_get_streof(in, 5, &cmd->sql_len);

    in_slash_comment = 0;
    in_dash_comment = 0;
    in_hash_comment = 0;
    in_double_quote = 0;
    in_single_quote = 0;
    in_backtick = 0;
    in_ident = 0;
    in_non_ident = 0;

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
            /* Permitted characters in unquoted identifiers:
             *
             *   ASCII: [0-9,a-z,A-Z$_] (basic Latin letters, digits 0-9, dollar, underscore)
             *   Extended: U+0080 .. U+FFFF
             *
             * TODO not handling wide chars for now
             *
             * https://dev.mysql.com/doc/refman/8.0/en/identifiers.html */
            if (in_ident && !cmd_lex_sql_is_ident_char(c)) {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_IDENT, &ts, i);
                in_ident = 0;
            }
            if (in_non_ident && (cmd_lex_sql_is_ident_char(c) || isspace(c) || c == ';')) {
                cmd_lex_sql_emit_token(cmd, STMT_TOKEN_NON_IDENT, &ts, i);
                in_non_ident = 0;
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
            } else if ((b == '\0' || isspace(b)) && c == '#') {
                in_hash_comment = 1;
                ts = i;
            } else if ((b == '\0' || isspace(b)) && c == '-' && d == '-') {
                in_dash_comment = 1;
                ts = i;
                ++i;
            } else if (c == '/' && d == '*') {
                in_slash_comment = 1;
                ts = i;
                ++i;
            } else if (!in_ident && !in_non_ident) {
                if (cmd_lex_sql_is_ident_char(c)) {
                    in_ident = 1;
                    ts = i;
                } else if (c != ';' && !isspace(c)) {
                    in_non_ident = 1;
                    ts = i;
                }
            }
        }
        b = c;
    }

    final_type = -1;
    if (in_dash_comment || in_hash_comment) {
        final_type = STMT_TOKEN_COMMENT;
    } else if (in_ident) {
        final_type = STMT_TOKEN_IDENT;
    } else if (in_non_ident) {
        final_type = STMT_TOKEN_NON_IDENT;
    }
    if (final_type != -1) {
        cmd_lex_sql_emit_token(cmd, final_type, &ts, i);
    }
    cmd_lex_sql_end_stmt(cmd, &ss, i);
    cmd->stmt_cur = cmd->stmt_list;

    return MYSW_ERR;
}

static int cmd_lex_sql_emit_token(cmd_t *cmd, int type, size_t *start, size_t end) {
    stmt_t *stmt;

    stmt = cmd->stmt_parsing;

    if (!stmt) {
        stmt = calloc(1, sizeof(stmt_t));
        LL_APPEND(cmd->stmt_list, stmt);
        cmd->stmt_parsing = stmt;
    }

    if (end > *start) {
        if (type == STMT_TOKEN_COMMENT && stmt->hint == NULL && stmt->first == NULL) {
            /* Set hint to first comment we see */
            stmt->hint = cmd->sql + *start;
            stmt->hint_len = end - *start;
            client_set_hint(cmd->client, cmd->sql + *start, end - *start);
        }
        if (type == STMT_TOKEN_IDENT && stmt->first == NULL) {
            /* Set to first to first ident we see */
            stmt->first = cmd->sql + *start;
            stmt->first_len = end - *start;
        }
        if ((type == STMT_TOKEN_IDENT || type == STMT_TOKEN_BACKTICK) && stmt->first != NULL && cmd_stmt_is_use(stmt)) {
            /* If we get a 2nd ident token and this is a USE stmt, set db name */
            client_set_db_name(cmd->client, cmd->sql + *start, end - *start);
        }
    }

    *start = end;
    return MYSW_OK;
}

static int cmd_lex_sql_is_ident_char(char c) {
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '_'
        || c == '$';
}

static int cmd_lex_sql_end_stmt(cmd_t *cmd, size_t *start, size_t end) {
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

int cmd_stmt_is_use(stmt_t *stmt) {
    return stmt->first && strncasecmp(stmt->first, "USE", stmt->first_len) == 0 ? 1 : 0;
}

int cmd_is_targeting(cmd_t *cmd) {
    if (cmd->cmd_byte == MYSQLD_COM_INIT_DB) {
        /* COM_INIT_DB */
        return 1;
    } else if (cmd->cmd_byte == MYSQLD_COM_QUERY
        && cmd->stmt_cur
        && cmd_stmt_is_use(cmd->stmt_cur)
    ) {
        /* USE statement */
        return 1;
    }
    return 0;
}

int cmd_expects_response(cmd_t *cmd) {
    if (cmd->cmd_byte == MYSQLD_COM_STMT_SEND_LONG_DATA || cmd->cmd_byte == MYSQLD_COM_STMT_CLOSE) {
        return 0;
    }
    return 1;
}

int cmd_deinit(cmd_t *cmd) {
    stmt_t *stmt, *stmt_tmp;
    LL_FOREACH_SAFE(cmd->stmt_list, stmt, stmt_tmp) {
        LL_DELETE(cmd->stmt_list, stmt);
        free(stmt);
    }
    return MYSW_ERR;
}
