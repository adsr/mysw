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
#define MYSQLD_COM_INIT_DB                           0x02
#define MYSQLD_COM_QUIT                              0x01

extern char* opt_addr;
extern int opt_port;
extern int opt_backlog;
extern int opt_num_threads;
extern int opt_epoll_max_events;
extern int opt_epoll_timeout_ms;
extern int opt_read_size;

typedef struct _buf_t buf_t;
typedef struct _fdh_t fdh_t;
typedef struct _proxy_t proxy_t;
typedef struct _server_t server_t;
typedef struct _client_t client_t;
typedef struct _worker_t worker_t;
typedef struct _pool_t pool_t;
typedef unsigned char uchar;

struct _buf_t {
    char *data;
    size_t len;
    size_t cap;
};

struct _fdh_t {
    #define FDH_TYPE_PROXY  0
    #define FDH_TYPE_CLIENT 1
    #define FDH_TYPE_SERVER 2
    proxy_t *proxy;
    int fd;
    int type;
    union {
        proxy_t *proxy;
        client_t *client;
        server_t *server;
    } u;
    int (*fn_read_write)(fdh_t *self);
    int (*fn_process)(fdh_t *self);
    int (*fn_destroy)(fdh_t *self);
    buf_t in;
    buf_t out;
    size_t write_pos;
    int epoll_flags;
    int read_write_errno;
    int read_eof;
    int destroy;
    worker_t *worker;
};

struct _proxy_t {
    int epfd;
    worker_t *workers;
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

struct _client_t {
    #define CLIENT_STATE_UNKNOWN                   0
    #define CLIENT_STATE_CONNECTED                 1
    #define CLIENT_STATE_HANDSHAKE_RECEIVED_INIT   2
    #define CLIENT_STATE_HANDSHAKE_SENT_RESPONSE   3
    #define CLIENT_STATE_HANDSHAKE_RECEIVED_ERR    4
    #define CLIENT_STATE_COMMAND_READY             5
    proxy_t *proxy;
    int state;
    int in_transaction;
    fdh_t fdh_conn;
    fdh_t fdh_event;
    client_t *next;
};

struct _server_t {
    #define SERVER_STATE_UNKNOWN                     0
    #define SERVER_STATE_DISCONNECTED                1
    #define SERVER_STATE_CONNECTING                  2
    #define SERVER_STATE_CONNECTED                   3
    #define SERVER_STATE_HANDSHAKE_SENDING_INIT      4
    #define SERVER_STATE_HANDSHAKE_SENT_INIT         5
    #define SERVER_STATE_COMMAND_READY               6
    proxy_t *proxy;
    char *host;
    int port;
    int state;
    fdh_t fdh_conn;
    fdh_t fdh_event;
    fdh_t fdh_timer;
    server_t *next;
};

struct _pool_t {
    proxy_t *proxy;
    char *name; /* TODO char array */
    server_t *servers_connecting;
    server_t *servers_free;
    server_t *servers_reserved;
    UT_hash_handle hh;
};

/* adam@asx1c5:~/mysw$ grep -Ph '^\S+ \*?[a-z]+_[^\(]+\(' *.c  | sed 's@ {@;@g' */
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
int buf_get_void(buf_t *buf, size_t pos, void *dest, size_t len);
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
int client_destroy(fdh_t *fdh);
int fdh_watch(fdh_t *fdh);
int fdh_rewatch(fdh_t *fdh);
int fdh_unwatch(fdh_t *fdh);
int fdh_read_write(fdh_t *fdh);
int fdh_read(fdh_t *fdh);
int fdh_write(fdh_t *fdh);
int fdh_is_write_finished(fdh_t *fdh);
int fdh_is_writing(fdh_t *fdh);
int fdh_reset_rw_state(fdh_t *fdh);
int pool_new(proxy_t *proxy, char *name, pool_t **out_pool);
int pool_find(proxy_t *proxy, char *name, pool_t **out_pool);
int pool_fill(pool_t *pool, char *host, int port, int n);
int pool_destroy(pool_t *pool);
int server_new(proxy_t *proxy, char *host, int port, server_t **out_server);
int server_process(fdh_t *fdh);
int server_connect(server_t *server);
int server_process_connecting(server_t *server);
int server_process_connected(server_t *server);
int server_process_handshake_sending_init(server_t *server);
int server_process_handshake_sent_init(server_t *server);
int server_destroy(fdh_t *fdh);
int util_has_complete_mysql_packet(buf_t *in);
int util_calc_native_auth_response(const uchar *pass, size_t pass_len, const uchar *challenge_20, uchar *out_auth_response);
int worker_init(worker_t *worker, proxy_t *proxy);
int worker_spawn(worker_t *worker);
int worker_join(worker_t *worker);
int worker_deinit(worker_t *worker);
int worker_accept_conn(fdh_t *fdh);

/* https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html */
