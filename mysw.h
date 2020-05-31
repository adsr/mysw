#ifndef _MYSW_H
#define _MYSW_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <utlist.h>
#include <uthash.h>
#include <aco.h>

#define MYSW_VERSION_STR "0.2.0"
#define MYSW_OK  0
#define MYSW_ERR 1

#define MYSW_STATE_CLIENT_IS_CONNECTING  1
#define MYSW_STATE_CLIENT_IS_SENDING_CMD 2
#define MYSW_STATE_CLIENT_IS_RECVING_RES 3

#define MYSQLD_CLIENT_CONNECT_WITH_DB                0x00000008
#define MYSQLD_CLIENT_PLUGIN_AUTH                    0x00080000
#define MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 0x00200000
#define MYSQLD_CLIENT_PROTOCOL_41                    0x00000200
#define MYSQLD_CLIENT_SECURE_CONNECTION              0x00008000

#define MYSQLD_SERVER_STATUS_AUTOCOMMIT              0x0002
#define MYSQLD_SERVER_STATUS_IN_TRANS                0x0001
#define MYSQLD_SERVER_STATUS_IN_TRANS_READONLY       0x2000
#define MYSQLD_SERVER_MORE_RESULTS_EXISTS            0x0008
#define MYSQLD_SERVER_STATUS_CURSOR_EXISTS           0x0040
#define MYSQLD_SERVER_STATUS_LAST_ROW_SENT           0x0080

#define MYSQLD_COM_QUERY                             0x03
#define MYSQLD_COM_STMT_PREPARE                      0x16
#define MYSQLD_COM_STMT_SEND_LONG_DATA               0x18
#define MYSQLD_COM_STMT_CLOSE                        0x19
#define MYSQLD_COM_INIT_DB                           0x02
#define MYSQLD_COM_QUIT                              0x01

#define MYSQLD_OK                                    0x00
#define MYSQLD_EOF                                   0xfe
#define MYSQLD_ERR                                   0xff

#define if_err_return(rv, expr) if (((rv) = (expr)) != 0) return rv
#define if_err_break(rv, expr)  if (((rv) = (expr)) != 0) break
#define if_err(rv, expr)        if (((rv) = (expr)) != 0)

typedef struct _acceptor_t acceptor_t;
typedef struct _backend_t  backend_t;
typedef struct _buf_t      buf_t;
typedef struct _client_t   client_t;
typedef struct _fdo_t      fdo_t;
typedef struct _fdh_t      fdh_t;
typedef struct _mlock_t    mlock_t;
typedef struct _mthread_t  mthread_t;
typedef struct _mysw_t     mysw_t;
typedef struct _targeter_t targeter_t;
typedef struct _worker_t   worker_t;
typedef struct _cmd_t      cmd_t;
typedef struct _stmt_t     stmt_t;

struct _buf_t {
    uint8_t *data;
    size_t len;
    size_t cap;
};

struct _mthread_t {
    pthread_t thread;
    int created;
};

struct _mlock_t {
    pthread_spinlock_t spinlock;
    int created;
};

struct _fdo_t {
    struct epoll_event *event;
    aco_t *co;
    aco_share_stack_t *co_stack;
    void (*co_func)();
    void *co_arg;
};

struct _fdh_t {
    fdo_t *fdo;
    int fd;
};

struct _mysw_t {
    char *opt_addr;
    int opt_port;
    int opt_backlog;
    int opt_num_workers;
    int opt_num_acceptors;
    int opt_num_epoll_events;
    int opt_max_num_clients;
    int opt_worker_epoll_timeout_ms;
    int opt_acceptor_select_timeout_s;
    worker_t *workers;
    acceptor_t *acceptors;
    backend_t *backends;
    targeter_t *targeters;
    mthread_t signal_thread;

    client_t *clients;
    client_t *clients_unused;
    mlock_t clients_lock;

    int listenfd;
    int done_pipe[2];
    int done;
};

struct _acceptor_t {
    int num;
    mthread_t thread;
};

struct _worker_t {
    int num;
    mthread_t thread;
    int epollfd;
    struct epoll_event *events;
    aco_t *main_co;
};

struct _cmd_t {
    client_t *client;
    uint8_t cmd_byte;
    buf_t *payload;
    char *sql;
    size_t sql_len;
    stmt_t *stmt_list;
    stmt_t *stmt_cur;
    stmt_t *stmt_parsing;
};

struct _stmt_t {
    #define STMT_TOKEN_COMMENT   0
    #define STMT_TOKEN_STRING    1
    #define STMT_TOKEN_BACKTICK  2
    #define STMT_TOKEN_IDENT     3
    #define STMT_TOKEN_NON_IDENT 4
    char *sql;
    size_t sql_len;
    char *hint;
    size_t hint_len;
    char *first;
    size_t first_len;
    stmt_t *next;
};

struct _client_t {
    int num;
    int state;
    worker_t *worker;
    fdo_t fdo;
    fdh_t fdh_socket;
    fdh_t fdh_event;
    fdh_t fdh_timer;
    buf_t wbuf;
    buf_t rbuf;
    buf_t username;
    buf_t db_name;
    buf_t hint;
    cmd_t cmd;
    uint8_t last_sequence_id;
    uint16_t status_flags;
    client_t *next_unused;
};

struct _backend_t {
    char *host;
    int port;
    char *dbname;
    fdo_t fdo;
    fdh_t fdh_socket;
    fdh_t fdh_event;
    fdh_t fdh_timer;
};

struct _targeter_t {
    int num;
    fdh_t fdh_event;
    pthread_t thread;
    int thread_created;
};

int acceptor_create_all();
int acceptor_free_all();
int acceptor_join_all();

int client_create_all();
int client_set_db_name(client_t *client, char *db_name, size_t db_name_len);
int client_set_hint(client_t *client, char *hint, size_t hint_len);
int client_free_all();

int listener_create();
int listener_free();

int signal_create_thread();
int signal_join_thread();

int worker_create_all();
int worker_free_all();
int worker_join_all();

int buf_append_str_len(buf_t *buf, char *str, size_t len);
int buf_append_str(buf_t *buf, char *str);
int buf_append_u8(buf_t *buf, uint8_t i);
int buf_append_u8_repeat(buf_t *buf, uint8_t i, int repeat);
int buf_append_u16(buf_t *buf, uint16_t i);
int buf_append_u24(buf_t *buf, uint32_t i);
int buf_append_u32(buf_t *buf, uint32_t i);
int buf_append_u64(buf_t *buf, uint64_t i);
int buf_clear(buf_t *buf);
int buf_ensure_cap(buf_t *buf, size_t cap);
int buf_free(buf_t *buf);
char *buf_get_str(buf_t *buf, size_t pos);
char *buf_get_str0(buf_t *buf, size_t pos, size_t *opt_len);
char *buf_get_streof(buf_t *buf, size_t pos, size_t *opt_len);
char *buf_get_strx(buf_t *buf, size_t pos, size_t *opt_len, int until_eof);
uint8_t *buf_get(buf_t *buf, size_t pos);
int buf_copy_from(buf_t *buf, buf_t *other);
int buf_copy_to(buf_t *buf, size_t pos, void *dest, size_t len);
uint8_t buf_get_u8(buf_t *buf, size_t pos);
uint16_t buf_get_u16(buf_t *buf, size_t pos);
uint32_t buf_get_u24(buf_t *buf, size_t pos);
uint32_t buf_get_u32(buf_t *buf, size_t pos);
uint64_t buf_get_u64(buf_t *buf, size_t pos);
int buf_len(buf_t *buf);
int buf_set_u24(buf_t *buf, size_t pos, uint32_t i);
int buf_set_u32(buf_t *buf, size_t pos, uint32_t i);
int buf_set_void(buf_t *buf, size_t pos, void *data, size_t len);
int buf_append_buf(buf_t *buf, buf_t *other);
int buf_append_void(buf_t *buf, void *data, size_t len);
int buf_assign_str_len(buf_t *buf, char *str, size_t len);
uint64_t buf_get_int_lenenc(buf_t *buf, size_t pos, int *len);

int cmd_init_and_parse(client_t *client, buf_t *in, cmd_t *cmd);
int cmd_stmt_is_use(stmt_t *stmt);
int cmd_is_targeting(cmd_t *cmd);
int cmd_expects_response(cmd_t *cmd);
int cmd_deinit(cmd_t *cmd);

extern mysw_t mysw;

#endif
