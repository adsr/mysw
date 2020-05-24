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


int util_calc_native_auth_response(const uchar *pass, size_t pass_len, const uchar *challenge_20, uchar *out_auth_response) {
    int i;
    uchar sha1_challenge[SHA_DIGEST_LENGTH];
    uchar sha1_pass[SHA_DIGEST_LENGTH];
    uchar sha1_sha1_pass[SHA_DIGEST_LENGTH];
    uchar challenge_plus_sha1_sha1_pass[20 + SHA_DIGEST_LENGTH];
    uchar sha1_challenge_plus_sha1_sha1_pass[SHA_DIGEST_LENGTH];

    /* https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_connection_phase_authentication_methods_native_password_authentication.html */
    /* The network packet content for the password is calculated by:
     * SHA1(password) XOR SHA1("20-bytes random data from server" <concat> SHA1(SHA1(password))) */
    SHA1(challenge_20, 20, sha1_challenge);

    SHA1(pass, pass_len, sha1_pass);
    SHA1(sha1_pass, sizeof(sha1_pass), sha1_sha1_pass);

    memcpy(challenge_plus_sha1_sha1_pass,      challenge_20,   20);
    memcpy(challenge_plus_sha1_sha1_pass + 20, sha1_sha1_pass, SHA_DIGEST_LENGTH);
    SHA1(challenge_plus_sha1_sha1_pass, sizeof(challenge_plus_sha1_sha1_pass), sha1_challenge_plus_sha1_sha1_pass);

    for (i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        *(out_auth_response + i) = sha1_pass[i] ^ sha1_challenge_plus_sha1_sha1_pass[i];
    }

    return MYSW_OK;
}
