// Protocol compatibility helpers. Xiaomi's cloud protocol requires RC4-drop1024.
#include "mbedtls/md.h"
#include "sodium.h"
#include "xrtc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *json_string(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
char *url_encode(const char *s) {
    size_t n = strlen(s);
    if (n > 256 * 1024)
        return NULL;
    char *out = malloc(n * 3 + 1), *p = out;
    if (!out)
        return NULL;
    const char *hex = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr("-_.~", c))
            *p++ = c;
        else if (c == ' ')
            *p++ = '+';
        else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 15];
        }
    }
    *p = 0;
    return out;
}
char *b64_encode(const uint8_t *data, size_t size) {
    size_t n = sodium_base64_ENCODED_LEN(size, sodium_base64_VARIANT_ORIGINAL);
    char *out = malloc(n);
    if (out)
        sodium_bin2base64(out, n, data, size, sodium_base64_VARIANT_ORIGINAL);
    return out;
}
uint8_t *b64_decode(const char *s, size_t *size) {
    size_t max = strlen(s) * 3 / 4 + 4;
    uint8_t *out = malloc(max);
    if (!out)
        return NULL;
    if (sodium_base642bin(out, max, s, strlen(s), "\r\n", size, NULL,
                          sodium_base64_VARIANT_ORIGINAL)) {
        free(out);
        return NULL;
    }
    return out;
}
void hash_bytes(const char *algorithm, const void *data, size_t size, uint8_t *out) {
    mbedtls_md_type_t type = !strcmp(algorithm, "MD5")    ? MBEDTLS_MD_MD5
                             : !strcmp(algorithm, "SHA1") ? MBEDTLS_MD_SHA1
                                                          : MBEDTLS_MD_SHA256;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    if (!info || mbedtls_md(info, data, size, out))
        abort();
}
void rc4_drop1024(const uint8_t key[32], const uint8_t *input, uint8_t *output, size_t size) {
    uint8_t state[256];
    unsigned i, j = 0;
    for (i = 0; i < 256; i++)
        state[i] = i;
    for (i = 0; i < 256; i++) {
        j = (j + state[i] + key[i % 32]) & 255;
        uint8_t tmp = state[i];
        state[i] = state[j];
        state[j] = tmp;
    }
    i = j = 0;
    for (size_t k = 0; k < size + 1024; k++) {
        i = (i + 1) & 255;
        j = (j + state[i]) & 255;
        uint8_t tmp = state[i];
        state[i] = state[j];
        state[j] = tmp;
        uint8_t byte = state[(state[i] + state[j]) & 255];
        if (k >= 1024)
            output[k - 1024] = input[k - 1024] ^ byte;
    }
    sodium_memzero(state, sizeof(state));
}
