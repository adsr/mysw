#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <utlist.h>

#define MYSW_VERSION_STR "0.1.0"

#define MYSQLD_CLIENT_CONNECT_WITH_DB                0x00000008
#define MYSQLD_CLIENT_PLUGIN_AUTH                    0x00080000
#define MYSQLD_CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA 0x00200000
#define MYSQLD_CLIENT_PROTOCOL_41                    0x00000200
#define MYSQLD_CLIENT_SECURE_CONNECTION              0x00008000
#define MYSQLD_SERVER_STATUS_AUTOCOMMIT              0x00000002

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
    int type; /* FDH_TYPE_... */
    int skip_read_write;
    union {
        proxy_t *proxy;
        client_t *client;
        server_t *server;
    } u;
    int fd;
    buf_t in;
    buf_t out;
    size_t out_cur;
    int epoll_flags;
    int last_errno;
    int eof;
};

struct _proxy_t {
    int epfd;
    int sockfd;
    worker_t *workers;
    pool_t *pool_map;
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
    #define CLIENT_STATE_CONNECTED                 0
    #define CLIENT_STATE_HANDSHAKE_RECEIVED_INIT   1
    #define CLIENT_STATE_HANDSHAKE_SENT_RESPONSE   2
    #define CLIENT_STATE_HANDSHAKE_RECEIVED_ERR    3
    #define CLIENT_STATE_COMMAND_READY             4
    #define CLIENT_STATE_COMMAND_AWAITING_RESPONSE 5
    proxy_t *proxy;
    int state;
    int in_transaction;
    fdh_t fdh_conn;
    fdh_t fdh_event;
    client_t *next;
};

struct _server_t {
    #define SERVER_STATE_CONNECTED                   0
    #define SERVER_STATE_HANDSHAKE_SENT_INIT         1
    #define SERVER_STATE_HANDSHAKE_RECEIVED_RESPONSE 2
    #define SERVER_STATE_HANDSHAKE_SENT_ERR          3
    #define SERVER_STATE_COMMAND_READY               4
    #define SERVER_STATE_COMMAND_RECEIVED            5
    proxy_t *proxy;
    char *host;
    int port;
    int state;
    fdh_t fdh_conn;
    fdh_t fdh_event;
    server_t *next;
};

struct pool_t {
    proxy_t *proxy;
    char *name;
    server_t *servers_free;
    server_t *servers_reserved;
    pool_t *next;
};

int worker_init(worker_t *worker, proxy_t *proxy);
int worker_spawn(worker_t *worker);
int worker_join(worker_t *worker);
int worker_deinit(worker_t *worker);

int client_new(worker_t *worker, int connfd, client_t **out_client);
int client_process(client_t *client);

int util_has_complete_mysql_packet(buf_t *in);

int fdh_watch(fdh_t *fdh);
int fdh_rewatch(fdh_t *fdh);
int fdh_unwatch(fdh_t *fdh);
int fdh_read_write(fdh_t *fdh);
int fdh_write(fdh_t *fdh);
int fdh_is_write_finished(fdh_t *fdh);
int fdh_is_writing(fdh_t *fdh);
int fdh_reset_rw_state(fdh_t *fdh);
int fdh_read(fdh_t *fdh);

int buf_append_str_len(buf_t *buf, char *str, size_t len);
int buf_append_u8(buf_t *buf, uint8_t i);
int buf_append_u16(buf_t *buf, uint16_t i);
int buf_clear(buf_t *buf);
int buf_ensure_cap(buf_t *buf, size_t cap);
int buf_free(buf_t *buf);
char *buf_get_str(buf_t *buf, size_t pos);
char *buf_get_str0(buf_t *buf, size_t pos, size_t *opt_len);
char *buf_get_streof(buf_t *buf, size_t pos, size_t *opt_len);
char *buf_get_strx(buf_t *buf, size_t pos, size_t *opt_len, int until_eof);
int buf_get_void(buf_t *buf, size_t pos, void *dest, size_t len);
uint8_t buf_get_u8(buf_t *buf, size_t pos);
uint32_t buf_get_u24(buf_t *buf, size_t pos);
uint32_t buf_get_u32(buf_t *buf, size_t pos);
int buf_len(buf_t *buf);
int buf_set_u24(buf_t *buf, size_t pos, uint32_t i);
int buf_set_void(buf_t *buf, size_t pos, void *data, size_t len);
int buf_append_void(buf_t *buf, void *data, size_t len);


/* https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html */
