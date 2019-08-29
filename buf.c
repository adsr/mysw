#include "mysw.h"

int buf_append_str_len(buf_t *buf, char *str, size_t len) {
    return buf_append_void(buf, str, len);
}

int buf_append_str(buf_t *buf, char *str) {
    return buf_append_str_len(buf, str, strlen(str));
}

int buf_append_u8(buf_t *buf, uint8_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_append_u8_repeat(buf_t *buf, uint8_t i, int repeat) {
    int c, rv;
    for (c = 0; c < repeat; ++c) {
        rv = buf_append_void(buf, &i, sizeof(i));
        if (rv != MYSW_OK) return rv;
    }
    return MYSW_OK;
}

int buf_append_u16(buf_t *buf, uint16_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_append_u24(buf_t *buf, uint32_t i) {
    return buf_append_void(buf, &i, 3);
}

int buf_append_u32(buf_t *buf, uint32_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_append_u64(buf_t *buf, uint64_t i) {
    return buf_append_void(buf, &i, sizeof(i));
}

int buf_clear(buf_t *buf) {
    buf->len = 0;
    return MYSW_OK;
}

int buf_ensure_cap(buf_t *buf, size_t cap) {
    if (buf->cap < cap) {
        if (buf->cap < 16)  buf->cap = 16;
        buf->cap *= 2;
        if (buf->cap < cap) buf->cap = cap;
        buf->data = realloc(buf->data, buf->cap);
    }
    return MYSW_OK;
}

int buf_free(buf_t *buf) {
    if (buf->data) {
        free(buf->data);
        buf->data = NULL;
    }
    memset(buf, 0, sizeof(buf_t));
    return MYSW_OK;
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

uint8_t *buf_get(buf_t *buf, size_t pos) {
    if (pos >= buf->len) {
        return NULL;
    }
    return buf->data + pos;
}

int buf_copy_from(buf_t *buf, buf_t *other) {
    buf_clear(buf);
    buf_append_void(buf, other->data, other->len);
    return MYSW_OK;
}

int buf_copy_to(buf_t *buf, size_t pos, void *dest, size_t len) {
    if (pos + len > buf->len) {
        return MYSW_ERR;
    }
    memcpy(dest, buf->data + pos, len);
    return MYSW_OK;
}

uint8_t buf_get_u8(buf_t *buf, size_t pos) {
    uint8_t i;
    i = 0;
    buf_copy_to(buf, pos, &i, sizeof(i));
    return i;
}

uint16_t buf_get_u16(buf_t *buf, size_t pos) {
    uint16_t i;
    i = 0;
    buf_copy_to(buf, pos, &i, sizeof(i));
    return i;
}

uint32_t buf_get_u24(buf_t *buf, size_t pos) {
    uint32_t i;
    i = 0;
    buf_copy_to(buf, pos, &i, 3);
    return i;
}

uint32_t buf_get_u32(buf_t *buf, size_t pos) {
    uint32_t i;
    i = 0;
    buf_copy_to(buf, pos, &i, sizeof(i));
    return i;
}

uint64_t buf_get_u64(buf_t *buf, size_t pos) {
    uint64_t i;
    i = 0;
    buf_copy_to(buf, pos, &i, sizeof(i));
    return i;
}

int buf_len(buf_t *buf) {
    return buf->len;
}

int buf_set_u24(buf_t *buf, size_t pos, uint32_t i) {
    return buf_set_void(buf, pos, &i, 3);
}

int buf_set_u32(buf_t *buf, size_t pos, uint32_t i) {
    return buf_set_void(buf, pos, &i, sizeof(i));
}

int buf_set_void(buf_t *buf, size_t pos, void *data, size_t len) {
    if (pos + len > buf->len) {
        return MYSW_ERR;
    }
    memcpy(buf->data + pos, data, len);
    return MYSW_OK;
}

int buf_append_buf(buf_t *buf, buf_t *other) {
    return buf_append_void(buf, other->data, other->len);
}

int buf_append_void(buf_t *buf, void *data, size_t len) {
    buf_ensure_cap(buf, buf->len + len + 1);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    *(buf->data + buf->len) = '\0';
    return MYSW_OK;
}

int buf_assign_str_len(buf_t *buf, char *str, size_t len) {
    buf_clear(buf);
    return buf_append_str_len(buf, str, len);
}

uint64_t buf_get_int_lenenc(buf_t *buf, size_t pos, int *len) {
    uint8_t a;
    a = buf_get_u8(buf, pos);
    if (a < 0xfb) {
        *len = 1;
        return a;
    } else if (a == 0xfc) {
        *len = 3;
        return buf_get_u16(buf, pos + 1);
    } else if (a == 0xfd) {
        *len = 4;
        return buf_get_u24(buf, pos + 1);
    } else if (a == 0xfe) {
        *len = 9;
        return buf_get_u64(buf, pos + 1);
    }
    *len = 0; /* TODO invalid lenenc int */
    return 0;
}
