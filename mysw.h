#ifndef __MYSW_H
#define __MYSW_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
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
#define MYSQLD_SERVER_STATUS_AUTOCOMMIT              0x00000002

#define MYSQLD_OK                                    0x00
#define MYSQLD_EOF                                   0xfe
#define MYSQLD_ERR                                   0xff
#define MYSQLD_COM_QUERY                             0x03
#define MYSQLD_COM_STMT_PREPARE                      0x16
#define MYSQLD_COM_STMT_SEND_LONG_DATA               0x18
#define MYSQLD_COM_STMT_CLOSE                        0x19
#define MYSQLD_COM_INIT_DB                           0x02
#define MYSQLD_COM_QUIT                              0x01

#define try(__rv, __call) do { if (((__rv) = (__call)) != MYSW_OK) return (__rv); } while(0)

extern char* opt_addr;
extern int opt_port;
extern int opt_backlog;
extern int opt_num_threads;
extern int opt_epoll_max_events;
extern int opt_epoll_timeout_ms;
extern int opt_read_size;

typedef struct _buf_t buf_t;
typedef struct _fdh_t fdh_t;
typedef struct _fdpoll_t fdpoll_t;
typedef struct _proxy_t proxy_t;
typedef struct _worker_t worker_t;
typedef struct _client_t client_t;
typedef struct _server_t server_t;
typedef struct _pool_t pool_t;
typedef struct _targeter_t targeter_t;
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
    fdpoll_t *fdpoll;
    void *udata;
    #define FDH_TYPE_SOCKET 0
    #define FDH_TYPE_EVENT  1
    int type;
    int fd;
    int is_writeable;
    pthread_spinlock_t *spinlock;
    int (*fn_process)(fdh_t *);
    #define FDH_STATE_UNWATCHED 0
    #define FDH_STATE_WATCHED   1
    #define FDH_STATE_ONESHOT   2
    int state;
    buf_t buf;
    size_t buf_cursor;
    int epoll_events;
    int read_write_skip;
    int read_write_errno;
    int read_eof;
};

struct _proxy_t {
    fdpoll_t *fdpoll;
    worker_t *workers;
    targeter_t *targeter;
    pool_t *pool_map;
    pthread_t signal_thread;
    int done;
    fdh_t fdh_listen;
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
    #define STMT_TOKEN_COMMENT  0
    #define STMT_TOKEN_STRING   1
    #define STMT_TOKEN_BACKTICK 2
    #define STMT_TOKEN_WORD     3
    char *sql;
    size_t sql_len;
    char *comment;
    size_t comment_len;
    char *first;
    size_t first_len;
    char *second;
    size_t second_len;
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
    pthread_spinlock_t spinlock;
    int state;
    int in_txn;
    int in_prep_stmt;
    pool_t *target_pool;
    server_t *target_server;
    cmd_t cmd;
    buf_t cmd_result;
    fdh_t fdh_socket_in;
    fdh_t fdh_socket_out;
    fdh_t fdh_event;
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
    proxy_t *proxy;
    pthread_spinlock_t spinlock;
    char *host;
    int port;
    int state;
    client_t *target_client;
    fdh_t fdh_socket_in;
    fdh_t fdh_socket_out;
    fdh_t fdh_event;
    fdh_t fdh_timer;
};

struct _pool_t {
    proxy_t *proxy;
    pthread_spinlock_t spinlock;
    char *name;
    server_t *servers_all; /* TODO are these lists or maps? */
    server_t *servers_connecting;
    server_t *servers_free;
    server_t *servers_reserved;
    client_t *client_queue;
    fdh_t fdh_event;
    UT_hash_handle hh;
};

struct _targeter_t {
    proxy_t *proxy;
    pthread_spinlock_t spinlock;
    client_t *client_queue;
    fdh_t fdh_event;
};

/* grep -Ph '^\S+ \*?[a-z]+_[^\(]+\(' *.c  | sed 's@ {@;@g' */
int buf_append_str_len(buf_t *buf, char *str, size_t len);
int buf_append_u8(buf_t *buf, uint8_t i);
int buf_append_u8_repeat(buf_t *buf, uint8_t i, int repeat);
int buf_append_u16(buf_t *buf, uint16_t i);
int buf_append_u32(buf_t *buf, uint32_t i);
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
int buf_len(buf_t *buf);
int buf_set_u24(buf_t *buf, size_t pos, uint32_t i);
int buf_set_void(buf_t *buf, size_t pos, void *data, size_t len);
int buf_append_void(buf_t *buf, void *data, size_t len);
int client_new(proxy_t *proxy, int connfd, client_t **out_client);
int client_process(fdh_t *fdh);
int client_destroy(client_t *client);
int client_wakeup(client_t *client);
int cmd_init(client_t *client, buf_t *in, cmd_t *cmd);
int cmd_lex_sql(buf_t *in, cmd_t *cmd);
int cmd_lex_sql_emit_token(cmd_t *cmd, int type, size_t *start, size_t end);
int cmd_lex_sql_end_stmt(cmd_t *cmd, size_t *start, size_t end);
int cmd_is_targeting(cmd_t *cmd);
int cmd_is_use_stmt(cmd_t *cmd);
int cmd_deinit(cmd_t *cmd);
int cmd_process_target(cmd_t *cmd);
int fdpoll_new(void *udata, fdpoll_t **out_fdpoll);
int fdpoll_free(fdpoll_t *fdpoll);
int fdpoll_event_loop(fdpoll_t *fdpoll);
int fdh_init(fdh_t *r, fdh_t *w, fdpoll_t *fdpoll, void *udata, pthread_spinlock_t *spinlock, int type, int rfd, int (*fn_process)(fdh_t *));
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
int fdh_reset_rw_state(fdh_t *fdh);
int pool_new(proxy_t *proxy, char *name, pool_t **out_pool);
int pool_find(proxy_t *proxy, char *name, pool_t **out_pool);
int pool_fill(pool_t *pool, char *host, int port, int n);
int pool_destroy(pool_t *pool);
int pool_queue_client(pool_t *pool, client_t *client);
int pool_wakeup(pool_t *pool);
int server_new(proxy_t *proxy, char *host, int port, server_t **out_server);
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
int server_set_client(server_t *server, client_t *client);
int server_wakeup(server_t *server);
int server_destroy(fdh_t *fdh);
int targeter_new(proxy_t *proxy, targeter_t **out_targeter);
int targeter_process(fdh_t *fdh);
int targeter_queue_client(targeter_t *targeter, client_t *client);
int targeter_wakeup(targeter_t *targeter);
int util_has_complete_mysql_packet(buf_t *in);
int util_calc_native_auth_response(const uchar *pass, size_t pass_len, const uchar *challenge_20, uchar *out_auth_response);
int worker_init(worker_t *worker, proxy_t *proxy);
int worker_spawn(worker_t *worker);
int worker_join(worker_t *worker);
int worker_deinit(worker_t *worker);
int worker_accept_conn(fdh_t *fdh);

extern server_t *server_a;
extern server_t *server_b;

/* https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html */

/*
states

client
- waiting for handshake packet
- sending handshake response
- waiting for handshake okay
- 

server


function get_pools(ctx)
  return {
    shardpile_001={
      {100, "shardpile001a:3306"}
    }
  }
end

function get_target(in, out)
  in["sql"]
  in["sql_command"]
  in["sql_comment"]
  in["db_name"]
  in["client_user"]
  in["client_ip"]

  if in["db_name"] == "etsy_shard"
    if in["response:shard"]
      target = target_for_shard(in["shard"])
      out["pool"] = target["pool"]
      out["db_name"] = target["db_name"]
      return OK
    elseif
    index_table, shard_column, shardifier_column, shardifier_val = parse_comment(in["comment"])
    out["request:shard"] = "db:shardpile_001:select shop_shard from shop_index where shop_id = hard"
    return OK
  end

  out["pool"] = "shardpile_001"
  out["db_name"] = in["db_name"]
  ...
  out["request:curl:a"] = "http://..."
  ...
  out["request:db:shard"] = "shardpile_001:select shop_shard from shop_index where shop_id"
  out["db_name"] = in["db_name"]
  return OK
end

==============================

CLIENT
                    CLIENT_UNKNOWN
(1) <- hello        CLIENT_RECV_HANDSHAKE_INIT          on sockfd writeable
(2) hello_res ->    CLIENT_SEND_HANDSHAKE_INIT_RES      on sockfd readable
(3) <- ok_err       CLIENT_RECV_HANDSHAKE_INIT_OK       on sockfd writeable
(4) cmd ->          CLIENT_SEND_CMD                     on sockfd readable
                    if need specific mysqld (txn, prepared stmt, etc):
                      (set server.cmd + write eventfd)
                    elif has pool:
                      (add to pool.queue + write eventfd)
                    else need targeting:
                      (add to targeter.queue + write eventfd)
(5) <- cmd_res      CLIENT_WAIT_CMD_RES                 on client.eventfd readable (triggered by SERVER_SEND_CMD_RES)
                    (client.stmt = client.stmt->next, goto CLIENT_SEND_CMD, else CLIENT_RECV_CMD_RES)
                    CLIENT_RECV_CMD_RES                 on sockfd writeable
                    CLIENT_DISCONNECTED


TARGETER
                    TARGETER_UNKNOWN
                    TARGETER_WAIT_TARGET_REQ             on eventfd readable (triggered by CLIENT_SEND_CMD)
                    TARGETER_SEND_TARGET_REQ             on targetfd writeable
                    TARGETER_RECV_TARGET_RES             on targetfd readable
                    (add cmd to pool.queue + write eventfd)
                    (always wait on eventfd, wait on targetfd writeable if queue>0, wait on targetfd readable if waiting>0)

SERVER
                    SERVER_UNKNOWN
                    SERVER_WAIT_CONNECT         on server.timerfd readable (triggered by SERVER_DISCONNECTED)
(0)                 SERVER_CONNECTING           on sockfd readable
(1) <- hello        SERVER_SEND_HELLO           on sockfd readable
(2) hello_res ->    SERVER_RECV_HELLO_RES       on sockfd writeable
(3) <- ok_err       SERVER_SEND_HELLO_OK        on sockfd readable
                    (move to pool.free + write pool.eventfd)
(4) cmd ->          SERVER_WAIT_CMD             on server.eventfd readable (triggered by POOL_WAIT_DEQUEUE or CLIENT_SEND_CMD)
                    SERVER_RECV_CMD             on sockfd writeable
(5) <- cmd_res      SERVER_SEND_CMD_RES         on sockfd readable
                    (mark server.cmd.processed)
                    (write client.eventfd)
                    (move to pool.free + write pool.eventfd unless in_txn or prepared stmt)
                    SERVER_DISCONNECTED
                    (requeue server.cmd if not .processed)

POOL
                    POOL_UNKNOWN
                    POOL_WAIT_DEQUEUE           pool.eventfd readable (triggered by SERVER_SEND_HELLO_OK, SERVER_SEND_CMD_RES, CLIENT_SEND_CMD)
                    (dequeue from pool.queue)
                    (find server in pool.free, move to pool.reserved, set server.cmd, write server.eventfd)
                    
*/

/*

epoll change

always add socket.fd with epollin
always add event.fd with epollin
for writing...

write immediately until EAGAIN/EWOULDBLOCK, then add fd with epollout

if something to write
    write
    if EAGAIN/EWOULDBLOCK, add fd with epollout
elif events & epollout and something to write
    write
    if finished, del fd
elif events & epollout and nothing to write
   del fd

*/

#endif
