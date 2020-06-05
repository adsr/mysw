#include "mysw.h"

int targeter_queue_request(client_t *client) {
    // TODO
    buf_assign_str_len(&client->target_pool, "pool_a", 4);
    client_wakeup(client);
    return MYSW_OK;
}
