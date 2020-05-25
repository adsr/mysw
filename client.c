#include "mysw.h"

void client_handle() {
    client_t *client;
    fdh_t *fdh;
    uint64_t u64;
    char buf[1024];

    while (!mysw.done) {
        client = (client_t*)aco_get_arg();
        fdh = (fdh_t*)client->fdo.event->data.ptr;
        buf[0] = '\0';

        if (fdh == &client->fdh_event) {
            read(client->fdh_event.fd, &u64, sizeof(u64));
            printf("eventfd u64=%lu\n", u64);
        } else if (fdh == &client->fdh_socket) {
            read(client->fdh_socket.fd, buf, sizeof(buf));
            printf("socketfd buf=%s\n", buf);
        }

        aco_yield();
    }

    client->fdo.co_dead = 1;
    aco_exit();
}
