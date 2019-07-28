#include "mysw.h"

int util_has_complete_mysql_packet(buf_t *in) {
    size_t payload_len;

    if (in->len < 4) {
        /* No header yet (incomplete) */
        return 0;
    }

    /* First 3 bytes are the payload length (then a sequence byte) */
    payload_len = buf_get_u24(in, 0);

    if (in->len - 4 >= payload_len) {
        /* Complete */
        return 1;
    }

    /* Incomplete */
    return 0;
}
