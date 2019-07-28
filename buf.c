#include "mysw.h"

int buf_append_str_len(buf_t *buf, char *str, size_t len) {
    return buf_append_void(buf, str, len);
}

int buf_append_u8(buf_t *buf, uint8_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_append_u16(buf_t *buf, uint16_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_clear(buf_t *buf) {
    buf->len = 0;
    return 0;
}

int buf_ensure_cap(buf_t *buf, size_t cap) {
    if (cap > buf->cap) {
        buf->data = realloc(buf->data, cap);
        buf->cap = cap;
    }
    return 0;
}

int buf_free(buf_t *buf) {
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    memset(buf, 0, sizeof(buf_t));
    return 0;
}

char *buf_get_str(buf_t *buf, size_t pos) {
    return buf_get_str0(buf, pos, NULL);
}

char *buf_get_str0(buf_t *buf, size_t pos, size_t *opt_len) {
    return buf_get_strx(buf, pos, opt_len, 0);
}

char *buf_get_streof(buf_t *buf, size_t pos, size_t *opt_len) {
    return buf_get_strx(buf, pos, opt_len, 1);
}

char *buf_get_strx(buf_t *buf, size_t pos, size_t *opt_len, int until_eof) {
    char *str;
    if (pos > buf->len) {
        return NULL;
    }
    if (buf->len <= 0 || !buf->data) {
        str = "";
    } else {
        str = (char*)(buf->data + pos);
    }
    if (opt_len) {
        if (until_eof) {
            *opt_len = buf->len - pos;
        } else {
            *opt_len = strlen(str);
        }
    }
    return str;
}

int buf_get_void(buf_t *buf, size_t pos, void *dest, size_t len) {
    if (pos + len > buf->len) {
        return 1;
    }
    memcpy(dest, buf->data + pos, len);
    return 0;
}

uint8_t buf_get_u8(buf_t *buf, size_t pos) {
    uint8_t i;
    i = 0;
    return buf_get_void(buf, pos, &i, sizeof(i));
}

uint32_t buf_get_u24(buf_t *buf, size_t pos) {
    uint32_t i;
    i = 0;
    return buf_get_void(buf, pos, &i, 3);
}

uint32_t buf_get_u32(buf_t *buf, size_t pos) {
    uint32_t i;
    i = 0;
    return buf_get_void(buf, pos, &i, sizeof(i));
}

int buf_len(buf_t *buf) {
    return buf->len;
}

int buf_set_u24(buf_t *buf, size_t pos, uint32_t i) {
    return buf_set_void(buf, pos, &i, 3);
}

int buf_set_void(buf_t *buf, size_t pos, void *data, size_t len) {
    if (pos + len > buf->len) {
        return 1;
    }
    memcpy(buf->data + pos, data, len);
    return 0;
}

int buf_append_void(buf_t *buf, void *data, size_t len) {
    buf_ensure_cap(buf, buf->len + len + 1);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    *(buf->data + buf->len) = '\0';
    return 0;
}
