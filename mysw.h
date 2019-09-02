#ifndef __MYSW_H
#define __MYSW_H

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

#define MYSW_VERSION_STR "0.1.0"
#define MYSW_ERR 1
#define MYSW_OK  0

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

#define try(__rv, __call)       do { if (((__rv) = (__call)) != MYSW_OK) return (__rv); } while(0)
#define try_break(__rv, __call) do { if (((__rv) = (__call)) != MYSW_OK) break; } while(0)

extern char *opt_addr;
extern int opt_port;
extern int opt_backlog;
extern int opt_num_worker_threads;
extern int opt_epoll_max_events;
extern int opt_epoll_timeout_ms;
extern int opt_read_size;
extern char *opt_targeter_socket_path;

typedef struct _buf_t buf_t;
typedef struct _fdpoll_t fdpoll_t;
typedef struct _fdo_t fdo_t;
typedef struct _fdh_t fdh_t;
typedef struct _proxy_t proxy_t;
typedef struct _worker_t worker_t;
typedef struct _client_t client_t;
typedef struct _server_t server_t;
typedef struct _pool_t pool_t;
typedef struct _targeter_t targeter_t;
typedef struct _targlet_t targlet_t;
typedef struct _stmt_t stmt_t;
typedef struct _cmd_t cmd_t;
typedef unsigned char uchar;

struct _buf_t {
    uchar *data;
    size_t len;
    size_t cap;
};

struct _fdpoll_t {
    int epoll_fd;
    int epoll_flags;
    int done;
    void *udata;
};

struct _fdh_t {
    fdo_t *fdo;
    #define FDH_TYPE_SOCKET_IN  0x01
    #define FDH_TYPE_SOCKET_OUT 0x02
    #define FDH_TYPE_EVENT_IN   0x04
    #define FDH_TYPE_TIMER_IN   0x08
    int type;
    int fd;
    #define FDH_STATE_UNWATCHED 0
    #define FDH_STATE_WATCHED   1
    #define FDH_STATE_ONESHOT   2
    int state;
    buf_t buf;
    size_t buf_cursor;
    uint32_t epoll_events;
    int read_write_skip;
    int read_write_errno;
    int read_eof;
};

struct _fdo_t {
    fdpoll_t *fdpoll;
    void *udata;
    int *ustate;
    pthread_spinlock_t *uspinlock;
    int (*uprocess)(fdh_t *);
    fdh_t socket_in;
    fdh_t socket_out;
    fdh_t event_in;
    fdh_t timer_in;
};

struct _proxy_t {
    fdpoll_t *fdpoll;
    fdo_t fdo;
    worker_t *workers;
    targeter_t *targeter;
    pool_t *pool_map;
    pthread_spinlock_t *spinlock_pool_map;
    pthread_spinlock_t spinlock_pool_map_val;
    pthread_t *signal_thread;
    pthread_t signal_thread_val;
    int done;
};

struct _worker_t {
    proxy_t *proxy;
    pthread_t thread;
    int spawned;
    struct epoll_event *events;
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
    #define CLIENT_STATE_UNKNOWN                 0
    #define CLIENT_STATE_RECV_HANDSHAKE_INIT     1
    #define CLIENT_STATE_SEND_HANDSHAKE_INIT_RES 2
    #define CLIENT_STATE_RECV_HANDSHAKE_RES      3
    #define CLIENT_STATE_SEND_CMD                4
    #define CLIENT_STATE_WAIT_CMD_RES            5
    #define CLIENT_STATE_RECV_CMD_RES            6
    proxy_t *proxy;
    fdo_t fdo;
    int state;
    pthread_spinlock_t spinlock;
    uint64_t client_id;
    uint64_t request_id;
    char *target_pool_name;
    pool_t *target_pool;
    server_t *target_server;
    uint8_t last_sequence_id;
    uint16_t status_flags;
    uint64_t prep_stmt_count;
    cmd_t cmd;
    buf_t username;
    buf_t db_name;
    buf_t hint;
    buf_t cmd_result;
    client_t *next_in_pool;
    client_t *next_in_targeter;
};

struct _server_t {
    #define SERVER_STATE_UNKNOWN                     0
    #define SERVER_STATE_DISCONNECTED                1
    #define SERVER_STATE_CONNECT                     2
    #define SERVER_STATE_CONNECTING                  3
    #define SERVER_STATE_CONNECTED                   4
    #define SERVER_STATE_SEND_HANDSHAKE_INIT         5
    #define SERVER_STATE_RECV_HANDSHAKE_INIT_RES     6
    #define SERVER_STATE_SEND_HANDSHAKE_RES          7
    #define SERVER_STATE_WAIT_CLIENT                 8
    #define SERVER_STATE_RECV_CMD                    9
    #define SERVER_STATE_SEND_CMD_RES                10
    pool_t *pool;
    fdo_t fdo;
    int state;
    pthread_spinlock_t spinlock;
    char *host;
    int port;
    char *dbname;
    client_t *target_client;
    int in_txn;
    int has_more_results;
    int has_prep_stmt;
    uint64_t ok_eof_err_count;
    int in_dead;
    int in_free;
    int in_reserved;
    server_t *next_in_dead;
    server_t *next_in_free;
    server_t *next_in_reserved;
};

struct _pool_t {
    proxy_t *proxy;
    fdo_t fdo;
    int state;
    pthread_spinlock_t spinlock_server_lists;
    pthread_spinlock_t spinlock_client_queue;
    char *name;
    server_t *servers_dead;
    server_t *servers_free;
    server_t *servers_reserved;
    client_t *client_queue;
    UT_hash_handle hh;
};

struct _targlet_t {
    targeter_t *targeter;
    int efd;
    /* TODO lua state */
    client_t *client;
    targlet_t *next_in_free;
    UT_hash_handle hh_in_reserved;
};

struct _targeter_t {
    proxy_t *proxy;
    fdo_t fdo;
    pthread_spinlock_t spinlock;
    client_t *client_queue;
    targlet_t *targlets_free;
    targlet_t *targlets_reserved;
};

/*
grep -Ph '^\S+ \*?[a-z]+_[^\(]+\(' *.c  | sed 's@ {@;@g'
*/
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
int client_new(proxy_t *proxy, int sockfd, client_t **out_client);
int client_destroy(client_t *client);
int client_process(fdh_t *fdh);
int client_write_ok_packet(client_t *client);
int client_write_err_packet(client_t *client, const char *err_fmt, ...);
int client_wakeup(client_t *client);
int client_set_db_name(client_t *client, char *db_name, size_t db_name_len);
int client_set_hint(client_t *client, char *hint, size_t hint_len);
int cmd_init(client_t *client, buf_t *in, cmd_t *cmd);
int cmd_stmt_is_use(stmt_t *stmt);
int cmd_is_targeting(cmd_t *cmd);
int cmd_expects_response(cmd_t *cmd);
int cmd_deinit(cmd_t *cmd);
int fdpoll_new(void *udata, fdpoll_t **out_fdpoll);
int fdpoll_free(fdpoll_t *fdpoll);
int fdpoll_event_loop(fdpoll_t *fdpoll);
int fdo_init(fdo_t *fdo, fdpoll_t *fdpoll, void *udata, int *ustate, pthread_spinlock_t *uspinlock, int sockfd, int eventfd, int timerfd, int (*uprocess)(fdh_t *));
int fdo_init_socket(fdo_t *fdo, int sockfd);
int fdo_deinit(fdo_t *fdo);
int fdo_set_state(fdo_t *fdo, int state, int watch_types);
int fdh_init(fdh_t *fdh, fdo_t *fdo, int type, int fd);
int fdh_deinit(fdh_t *fdh);
int fdh_ensure_watched(fdh_t *fdh);
int fdh_ensure_unwatched(fdh_t *fdh);
int fdh_watch(fdh_t *fdh);
int fdh_unwatch(fdh_t *fdh);
int fdh_read(fdh_t *fdh);
int fdh_read_event(fdh_t *fdh);
int fdh_read_socket(fdh_t *fdh);
int fdh_write(fdh_t *fdh);
int fdh_is_write_finished(fdh_t *fdh);
int fdh_is_write_unfinished(fdh_t *fdh);
int fdh_is_writing(fdh_t *fdh);
int fdh_is_writeable(fdh_t *fdh);
int fdh_clear(fdh_t *fdh);
int pool_new(proxy_t *proxy, char *name, pool_t **out_pool);
int pool_process(fdh_t *fdh);
int pool_find(proxy_t *proxy, char *name, size_t name_len, pool_t **out_pool);
int pool_fill(pool_t *pool, char *host, int port, char *dbname, int n);
int pool_server_reserve(pool_t *pool, server_t **out_server);
int pool_server_move_to_dead(pool_t *pool, server_t *server);
int pool_server_move_to_free(pool_t *pool, server_t *server);
int pool_server_move_to_reserved(pool_t *pool, server_t *server);
int pool_server_remove_from_list(pool_t *pool, server_t *server);
int pool_queue_client(pool_t *pool, client_t *client);
int pool_wakeup(pool_t *pool);
int pool_destroy(pool_t *pool);
int server_new(pool_t *pool, char *host, int port, char *dbname, server_t **out_server);
int server_process(fdh_t *fdh);
int server_process_disconnected(server_t *server);
int server_process_connect(server_t *server);
int server_process_connecting(server_t *server);
int server_process_send_handshake_init(server_t *server);
int server_process_recv_handshake_init_res(server_t *server);
int server_process_send_handshake_res(server_t *server);
int server_process_wait_client(server_t *server);
int server_process_recv_cmd(server_t *server);
int server_process_send_cmd_res(server_t *server);
int server_wakeup(server_t *server);
int server_destroy(server_t *server);
int targeter_new(proxy_t *proxy, targeter_t **out_targeter);
int targeter_spawn_targlet_threads(targeter_t *targeter);
int targeter_process(fdh_t *fdh);
int targeter_target_client(targeter_t *targeter, client_t *client);
int targeter_pool_client(targeter_t *targeter, client_t *client);
int targeter_queue_client_from_targlet(targeter_t *targeter, client_t *client, targlet_t *targlet);
int targeter_queue_client(targeter_t *targeter, client_t *client);
int targlet_wakeup(targlet_t *targlet);
int util_has_complete_mysql_packet(buf_t *in);
int util_calc_native_auth_response(const uchar *pass, size_t pass_len, const uchar *challenge_20, uchar *out_auth_response);
int worker_init(worker_t *worker, proxy_t *proxy);
int worker_spawn(worker_t *worker);
int worker_join(worker_t *worker);
int worker_deinit(worker_t *worker);
int worker_accept_conn(fdh_t *fdh);

extern server_t *server_a;
extern server_t *server_b;
extern pool_t *pool_a;

/* 

https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html

TODO list
- write targlets
- preallocate clients, targlets in addition to servers
- make dns lookup async
- use getopt
- max out wait_timeout on server connect (new states)
- audit error checking everywhere
- write unit tests for sql parsing
- write integration tests
- test client/server init/deinit disconnect/reconnect/failure modes
- audit locks
*/

#endif
