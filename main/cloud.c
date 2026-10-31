// C port of the Xiaomi account/MISS cloud protocol documented by go2rtc.
// See THIRD_PARTY_NOTICES.md. Credentials and HTTP bodies are never logged.
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "sodium.h"
#include "xrtc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACCOUNT "https://account.xiaomi.com"
#define MAX_RESPONSE (256 * 1024)
typedef struct {
    char user_id[32], cuser_id[128], service_token[2048], pass_token[1024];
    char ick[512], identity[1024], location[2048];
    uint8_t security[64];
    size_t security_len;
    char username[128], password[128], captcha[64];
    int flag;
    bool ready;
} cloud_t;
static cloud_t cloud;
static SemaphoreHandle_t mutex;
extern const uint8_t ca_start[] asm("_binary_cacrt_all_pem_start");
extern const uint8_t ca_end[] asm("_binary_cacrt_all_pem_end");
typedef struct {
    uint8_t *body;
    size_t length;
    bool failed;
} response_t;

static cJSON *failure(const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", message);
    return o;
}
static void cookie(const char *s) {
    struct {
        const char *key;
        char *target;
        size_t cap;
    } fields[] = {
        {"userId", cloud.user_id, sizeof(cloud.user_id)},
        {"cUserId", cloud.cuser_id, sizeof(cloud.cuser_id)},
        {"serviceToken", cloud.service_token, sizeof(cloud.service_token)},
        {"passToken", cloud.pass_token, sizeof(cloud.pass_token)},
        {"ick", cloud.ick, sizeof(cloud.ick)},
        {"identity_session", cloud.identity, sizeof(cloud.identity)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        size_t n = strlen(fields[i].key);
        if (strncmp(s, fields[i].key, n) || s[n] != '=')
            continue;
        const char *v = s + n + 1, *end = strchr(v, ';');
        size_t length = end ? (size_t)(end - v) : strlen(v);
        if (length < fields[i].cap) {
            memcpy(fields[i].target, v, length);
            fields[i].target[length] = 0;
        }
    }
}
static void security_from_json(cJSON *j) {
    const char *s = json_string(j, "ssecurity");
    if (*s) {
        size_t n;
        uint8_t *data = b64_decode(s, &n);
        if (data && n <= sizeof(cloud.security)) {
            memcpy(cloud.security, data, n);
            cloud.security_len = n;
        }
        free(data);
    }
    s = json_string(j, "passToken");
    if (*s && strlen(s) < sizeof(cloud.pass_token))
        strcpy(cloud.pass_token, s);
}
static esp_err_t on_http(esp_http_client_event_t *event) {
    response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        if (!strcasecmp(event->header_key, "Set-Cookie"))
            cookie(event->header_value);
        else if (!strcasecmp(event->header_key, "Location"))
            strlcpy(cloud.location, event->header_value, sizeof(cloud.location));
        else if (!strcasecmp(event->header_key, "Extension-Pragma")) {
            cJSON *j = cJSON_Parse(event->header_value);
            if (j) {
                security_from_json(j);
                cJSON_Delete(j);
            }
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response->length + event->data_len > MAX_RESPONSE) {
            response->failed = true;
            return ESP_FAIL;
        }
        uint8_t *p = realloc(response->body, response->length + event->data_len + 1);
        if (!p) {
            response->failed = true;
            return ESP_ERR_NO_MEM;
        }
        response->body = p;
        memcpy(p + response->length, event->data, event->data_len);
        response->length += event->data_len;
        p[response->length] = 0;
    }
    return ESP_OK;
}
static bool trusted_cloud_url(const char *url) {
    if (strncmp(url, "https://", 8))
        return false;
    const char *p = url + 8, *end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    const char *suffix[] = {".xiaomi.com", ".mi.com"};
    for (size_t i = 0; i < 2; i++) {
        size_t k = strlen(suffix[i]);
        if (n > k && !strncasecmp(p + n - k, suffix[i], k))
            return true;
    }
    return false;
}
static uint8_t *request(const char *url, const char *body, const char *cookies, bool follow,
                        size_t *length) {
    char *current = strdup(url);
    uint8_t *result = NULL;
    if (!current)
        return NULL;
    for (int redirect = 0; redirect < 8; redirect++) {
        if (!trusted_cloud_url(current))
            break;
        response_t response = {0};
        cloud.location[0] = 0;
        esp_http_client_config_t config = {
            .url = current,
            .timeout_ms = 15000,
            .use_global_ca_store = true,
            .disable_auto_redirect = true,
            .event_handler = on_http,
            .user_data = &response,
            .buffer_size = 4096,
            .buffer_size_tx = 4096,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client)
            break;
        if (body) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
            esp_http_client_set_post_field(client, body, strlen(body));
        }
        if (cookies && *cookies)
            esp_http_client_set_header(client, "Cookie", cookies);
        esp_http_client_set_header(client, "User-Agent", "xiaomi-rtc-esp32/" XRTC_VERSION);
        esp_err_t error = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (error != ESP_OK || response.failed) {
            free(response.body);
            break;
        }
        if (follow && status >= 300 && status < 400 && cloud.location[0]) {
            char next[2048];
            if (cloud.location[0] == '/')
                snprintf(next, sizeof(next), ACCOUNT "%s", cloud.location);
            else
                strlcpy(next, cloud.location, sizeof(next));
            free(current);
            current = strdup(next);
            free(response.body);
            body = NULL;
            if (!current)
                break;
            continue;
        }
        if (status < 200 || status >= 300) {
            free(response.body);
            break;
        }
        result = response.body;
        if (!result)
            result = (uint8_t *)strdup("");
        if (length)
            *length = response.length;
        break;
    }
    free(current);
    return result;
}
static cJSON *account_request(const char *url, const char *body, const char *cookies) {
    uint8_t *response = request(url, body, cookies, false, NULL);
    if (!response)
        return NULL;
    char *text = (char *)response;
    if (!strncmp(text, "&&&START&&&", 11))
        text += 11;
    cJSON *json = cJSON_Parse(text);
    free(response);
    return json;
}
static esp_err_t finish_login(const char *location) {
    if (!location || !*location)
        return ESP_FAIL;
    uint8_t *response = request(location, NULL, NULL, true, NULL);
    free(response);
    if (!response || !cloud.security_len || !cloud.service_token[0] || !cloud.user_id[0] ||
        !cloud.pass_token[0])
        return ESP_FAIL;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "user_id", cloud.user_id);
    cJSON_AddStringToObject(j, "pass_token", cloud.pass_token);
    esp_err_t error = config_update(j);
    cJSON_Delete(j);
    cloud.ready = error == ESP_OK;
    sodium_memzero(cloud.password, sizeof(cloud.password));
    return error;
}
static esp_err_t restore_login(void) {
    if (cloud.ready)
        return ESP_OK;
    xrtc_config_t cfg;
    config_get(&cfg);
    if (!cfg.user_id[0] || !cfg.pass_token[0]) {
        sodium_memzero(&cfg, sizeof(cfg));
        return ESP_ERR_INVALID_STATE;
    }
    char *cookies;
    asprintf(&cookies, "userId=%s; passToken=%s", cfg.user_id, cfg.pass_token);
    strlcpy(cloud.pass_token, cfg.pass_token, sizeof(cloud.pass_token));
    sodium_memzero(&cfg, sizeof(cfg));
    if (!cookies)
        return ESP_ERR_NO_MEM;
    cJSON *j = account_request(ACCOUNT "/pass/serviceLogin?_json=true&sid=xiaomiio", NULL, cookies);
    sodium_memzero(cookies, strlen(cookies));
    free(cookies);
    if (!j)
        return ESP_FAIL;
    security_from_json(j);
    esp_err_t error = finish_login(json_string(j, "location"));
    cJSON_Delete(j);
    return error;
}
static char *signature(const char *path, const char *data, const char *hash,
                       const uint8_t signed_nonce[32]) {
    char *nonce = b64_encode(signed_nonce, 32), *message = NULL;
    if (!nonce)
        return NULL;
    if (hash)
        asprintf(&message, "POST&%s&data=%s&rc4_hash__=%s&%s", path, data, hash, nonce);
    else
        asprintf(&message, "POST&%s&data=%s&%s", path, data, nonce);
    free(nonce);
    if (!message)
        return NULL;
    uint8_t digest[20];
    hash_bytes("SHA1", message, strlen(message), digest);
    free(message);
    return b64_encode(digest, sizeof(digest));
}
static cJSON *cloud_request(const char *path, const char *params) {
    if (restore_login() != ESP_OK)
        return NULL;
    uint8_t nonce[12], signed_nonce[32], material[76];
    esp_fill_random(nonce, 8);
    uint32_t minute = htonl((uint32_t)(time(NULL) / 60));
    memcpy(nonce + 8, &minute, 4);
    memcpy(material, cloud.security, cloud.security_len);
    memcpy(material + cloud.security_len, nonce, 12);
    hash_bytes("SHA256", material, cloud.security_len + 12, signed_nonce);
    char *first_hash = signature(path, params, NULL, signed_nonce);
    if (!first_hash)
        return NULL;
    size_t params_len = strlen(params), hash_len = strlen(first_hash);
    uint8_t *encrypted = malloc(params_len + 1), *encrypted_hash = malloc(hash_len + 1);
    if (!encrypted || !encrypted_hash) {
        free(encrypted);
        free(encrypted_hash);
        free(first_hash);
        return NULL;
    }
    rc4_drop1024(signed_nonce, (const uint8_t *)params, encrypted, params_len);
    rc4_drop1024(signed_nonce, (uint8_t *)first_hash, encrypted_hash, hash_len);
    char *data64 = b64_encode(encrypted, params_len),
         *hash64 = b64_encode(encrypted_hash, hash_len);
    free(first_hash);
    free(encrypted);
    free(encrypted_hash);
    if (!data64 || !hash64) {
        free(data64);
        free(hash64);
        return NULL;
    }
    char *sig = signature(path, data64, hash64, signed_nonce),
         *nonce64 = b64_encode(nonce, sizeof(nonce));
    char *ed = url_encode(data64), *eh = url_encode(hash64), *es = sig ? url_encode(sig) : NULL,
         *en = nonce64 ? url_encode(nonce64) : NULL;
    free(data64);
    free(hash64);
    free(sig);
    free(nonce64);
    if (!ed || !eh || !es || !en) {
        free(ed);
        free(eh);
        free(es);
        free(en);
        return NULL;
    }
    char *body = NULL, *cookies = NULL;
    asprintf(&body, "_nonce=%s&data=%s&rc4_hash__=%s&signature=%s", en, ed, eh, es);
    asprintf(&cookies, "userId=%s; cUserId=%s; serviceToken=%s", cloud.user_id, cloud.cuser_id,
             cloud.service_token);
    free(ed);
    free(eh);
    free(es);
    free(en);
    xrtc_config_t cfg;
    config_get(&cfg);
    char url[256];
    if (!strcmp(cfg.region, "cn"))
        snprintf(url, sizeof(url), "https://api.io.mi.com/app%s", path);
    else
        snprintf(url, sizeof(url), "https://%s.api.io.mi.com/app%s", cfg.region, path);
    sodium_memzero(&cfg, sizeof(cfg));
    uint8_t *response = body && cookies ? request(url, body, cookies, false, NULL) : NULL;
    free(body);
    free(cookies);
    if (!response) {
        cloud.ready = false;
        return NULL;
    }
    size_t length;
    uint8_t *decoded = b64_decode((char *)response, &length);
    free(response);
    if (!decoded)
        return NULL;
    rc4_drop1024(signed_nonce, decoded, decoded, length);
    decoded[length] = 0;
    cJSON *j = cJSON_Parse((char *)decoded);
    sodium_memzero(decoded, length);
    free(decoded);
    cJSON *code = cJSON_GetObjectItemCaseSensitive(j, "code"), *result = NULL;
    if (cJSON_IsNumber(code) && code->valueint == 0)
        result = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(j, "result"), true);
    cJSON_Delete(j);
    return result;
}
void cloud_init(void) {
    mutex = xSemaphoreCreateMutex();
    // Use full trusted root certificates so Mbed TLS can build alternative
    // paths when a server sends extra cross-signed roots. Keep hostname and
    // certificate validation enabled, using ESP-IDF's Mozilla trust source.
    ESP_ERROR_CHECK(esp_tls_init_global_ca_store());
    ESP_ERROR_CHECK(esp_tls_set_global_ca_store(ca_start, ca_end - ca_start));
}
esp_err_t cloud_camera_auth(camera_auth_t *auth) {
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(30000)))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_FAIL;
    crypto_box_keypair(auth->public_key, auth->secret_key);
    sodium_bin2hex(auth->public_hex, sizeof(auth->public_hex), auth->public_key,
                   sizeof(auth->public_key));
    xrtc_config_t cfg;
    config_get(&cfg);
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "app_pubkey", auth->public_hex);
    cJSON_AddStringToObject(params, "did", cfg.did);
    cJSON_AddStringToObject(params, "support_vendors", "TUTK_CS2_MTP");
    sodium_memzero(&cfg, sizeof(cfg));
    char *text = cJSON_PrintUnformatted(params);
    cJSON_Delete(params);
    cJSON *j = text ? cloud_request("/v2/device/miss_get_vendor", text) : NULL;
    free(text);
    cJSON *vendor =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "vendor"), "vendor");
    const char *pub = json_string(j, "public_key"), *sign = json_string(j, "sign");
    uint8_t device_public[32];
    size_t keylen;
    if (cJSON_IsNumber(vendor) && vendor->valueint != 4)
        result = ESP_ERR_NOT_SUPPORTED;
    else if (cJSON_IsNumber(vendor) && strlen(pub) == 64 && *sign &&
             strlen(sign) < sizeof(auth->sign) &&
             sodium_hex2bin(device_public, sizeof(device_public), pub, strlen(pub), NULL, &keylen,
                            NULL) == 0 &&
             keylen == 32 &&
             crypto_box_beforenm(auth->shared_key, device_public, auth->secret_key) == 0) {
        strcpy(auth->sign, sign);
        result = ESP_OK;
    }
    cJSON_Delete(j);
    xSemaphoreGive(mutex);
    return result;
}
cJSON *cloud_devices(void) {
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(30000)))
        return failure("Cloud is busy");
    cJSON *j = cloud_request("/v2/home/device_list_page", "{}"), *out = cJSON_CreateObject(),
          *items = cJSON_CreateArray();
    if (!j) {
        cJSON_Delete(out);
        cJSON_Delete(items);
        xSemaphoreGive(mutex);
        return failure("Xiaomi authentication or device-list request failed");
    }
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(j, "list")) {
        const char *model = json_string(item, "model");
        if (!strstr(model, ".camera.") && !strstr(model, ".cateye."))
            continue;
        cJSON *device = cJSON_CreateObject();
        const char *fields[] = {"did", "name", "model", "localip"};
        for (size_t i = 0; i < 4; i++)
            cJSON_AddStringToObject(device, fields[i], json_string(item, fields[i]));
        cJSON_AddItemToArray(items, device);
    }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddItemToObject(out, "devices", items);
    cJSON_Delete(j);
    xSemaphoreGive(mutex);
    return out;
}
static cJSON *captcha_challenge(const char *path) {
    char url[2048];
    if (path[0] == '/')
        snprintf(url, sizeof(url), ACCOUNT "%s", path);
    else
        strlcpy(url, path, sizeof(url));
    size_t n;
    uint8_t *image = request(url, NULL, NULL, false, &n);
    if (!image)
        return failure("Unable to load Xiaomi captcha");
    char *base64 = b64_encode(image, n), *data_url = NULL;
    free(image);
    if (base64)
        asprintf(&data_url, "data:image/jpeg;base64,%s", base64);
    free(base64);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", "captcha");
    cJSON_AddStringToObject(o, "captcha", data_url ? data_url : "");
    free(data_url);
    return o;
}
static const char *verify_name(void) {
    return cloud.flag == 4 ? "Phone" : cloud.flag == 8 ? "Email" : NULL;
}
static cJSON *send_ticket(void) {
    const char *name = verify_name();
    if (!name)
        return failure("Unsupported Xiaomi verification method");
    char url[512], cookies[1600];
    snprintf(url, sizeof(url), ACCOUNT "/identity/auth/verify%s?_flag=%d&_json=true", name,
             cloud.flag);
    snprintf(cookies, sizeof(cookies), "identity_session=%s; ick=%s", cloud.identity, cloud.ick);
    cJSON *j = account_request(url, NULL, cookies);
    char destination[128];
    snprintf(destination, sizeof(destination), "%s",
             json_string(j, cloud.flag == 4 ? "maskedPhone" : "maskedEmail"));
    cJSON_Delete(j);
    snprintf(url, sizeof(url), ACCOUNT "/identity/auth/send%sTicket", name);
    char *captcha = url_encode(cloud.captcha), *body = NULL;
    if (captcha)
        asprintf(&body, "_json=true&icode=%s&retry=0", captcha);
    free(captcha);
    j = body ? account_request(url, body, cookies) : NULL;
    free(body);
    cJSON *out;
    if (*json_string(j, "captchaURL"))
        out = captcha_challenge(json_string(j, "captchaURL"));
    else {
        cJSON *code = cJSON_GetObjectItemCaseSensitive(j, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 0)
            out = failure("Xiaomi could not send verification code");
        else {
            out = cJSON_CreateObject();
            cJSON_AddStringToObject(out, "state", "verify");
            cJSON_AddStringToObject(out, "destination", destination);
        }
    }
    cJSON_Delete(j);
    return out;
}
static cJSON *password_login(void) {
    cJSON *j = account_request(ACCOUNT "/pass/serviceLogin?_json=true&sid=xiaomiio", NULL, NULL);
    if (!j)
        return failure("Xiaomi login initialization failed");
    uint8_t digest[16];
    hash_bytes("MD5", cloud.password, strlen(cloud.password), digest);
    char hash[33];
    sodium_bin2hex(hash, sizeof(hash), digest, sizeof(digest));
    for (char *p = hash; *p; p++)
        if (*p >= 'a' && *p <= 'f')
            *p -= 32;
    const char *names[] = {"_json", "hash", "sid", "callback", "_sign", "qs", "user", "captCode"};
    const char *values[] = {"true",
                            hash,
                            json_string(j, "sid"),
                            json_string(j, "callback"),
                            json_string(j, "_sign"),
                            json_string(j, "qs"),
                            cloud.username,
                            cloud.captcha};
    char *body = calloc(1, 16384);
    size_t used = 0;
    if (!body) {
        cJSON_Delete(j);
        return failure("Out of memory");
    }
    for (size_t i = 0; i < 8; i++) {
        char *v = url_encode(values[i]);
        if (!v || used + strlen(v) + strlen(names[i]) + 3 >= 16384) {
            free(v);
            free(body);
            cJSON_Delete(j);
            return failure("Login request is too large");
        }
        used += snprintf(body + used, 16384 - used, "%s%s=%s", i ? "&" : "", names[i], v);
        free(v);
    }
    cJSON_Delete(j);
    char device_id[17];
    uint8_t random[8];
    esp_fill_random(random, sizeof(random));
    sodium_bin2hex(device_id, sizeof(device_id), random, sizeof(random));
    char cookies[640];
    snprintf(cookies, sizeof(cookies), "deviceId=%s; ick=%s", device_id, cloud.ick);
    j = account_request(ACCOUNT "/pass/serviceLoginAuth2", body, cookies);
    sodium_memzero(body, strlen(body));
    free(body);
    if (!j)
        return failure("Xiaomi login request failed");
    cJSON *out = NULL;
    if (*json_string(j, "captchaURL"))
        out = captcha_challenge(json_string(j, "captchaURL"));
    else if (*json_string(j, "notificationUrl")) {
        const char *url = json_string(j, "notificationUrl"),
                   *part = strstr(url, "/fe/service/identity/authStart");
        char *next = NULL;
        if (part)
            asprintf(&next, "%.*s/identity/list%s", (int)(part - url), url,
                     part + strlen("/fe/service/identity/authStart"));
        cJSON *identity = next ? account_request(next, NULL, NULL) : NULL;
        free(next);
        cJSON *flag = cJSON_GetObjectItemCaseSensitive(identity, "flag");
        cloud.flag = cJSON_IsNumber(flag) ? flag->valueint : 0;
        cJSON_Delete(identity);
        out = send_ticket();
    } else {
        security_from_json(j);
        if (finish_login(json_string(j, "location")) == ESP_OK) {
            out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "ok", true);
        } else
            out = failure("Xiaomi rejected login; check account, password and region");
    }
    cJSON_Delete(j);
    return out;
}
cJSON *cloud_action(const cJSON *request_json) {
    if (!xSemaphoreTake(mutex, pdMS_TO_TICKS(30000)))
        return failure("Cloud is busy");
    const char *action = json_string(request_json, "action");
    cJSON *out = NULL;
    if (!strcmp(action, "token")) {
        cloud.ready = false;
        if (config_update(request_json) == ESP_OK && restore_login() == ESP_OK) {
            out = cJSON_CreateObject();
            cJSON_AddBoolToObject(out, "ok", true);
        } else
            out = failure("Xiaomi account token is invalid or cloud is unavailable");
    } else if (!strcmp(action, "login")) {
        const char *user = json_string(request_json, "username"),
                   *pass = json_string(request_json, "password");
        if (!*user || !*pass || strlen(user) >= sizeof(cloud.username) ||
            strlen(pass) >= sizeof(cloud.password))
            out = failure("Invalid username or password length");
        else {
            cloud.ready = false;
            cloud.flag = 0;
            cloud.captcha[0] = 0;
            strcpy(cloud.username, user);
            strcpy(cloud.password, pass);
            out = password_login();
        }
    } else if (!strcmp(action, "captcha")) {
        const char *code = json_string(request_json, "code");
        if (!*code || strlen(code) >= sizeof(cloud.captcha))
            out = failure("Invalid captcha");
        else {
            strcpy(cloud.captcha, code);
            out = cloud.flag ? send_ticket() : password_login();
        }
    } else if (!strcmp(action, "verify")) {
        const char *name = verify_name(), *ticket = json_string(request_json, "code");
        if (!name || !*ticket || strlen(ticket) > 32)
            out = failure("No verification challenge is pending");
        else {
            char *encoded = url_encode(ticket), *url = NULL;
            char cookies[1200];
            if (encoded)
                asprintf(&url,
                         ACCOUNT
                         "/identity/auth/verify%s?_flag=%d&ticket=%s&trust=false&_json=true",
                         name, cloud.flag, encoded);
            free(encoded);
            snprintf(cookies, sizeof(cookies), "identity_session=%s", cloud.identity);
            cJSON *j = url ? account_request(url, "", cookies) : NULL;
            free(url);
            if (j && finish_login(json_string(j, "location")) == ESP_OK) {
                out = cJSON_CreateObject();
                cJSON_AddBoolToObject(out, "ok", true);
            } else
                out = failure("Xiaomi verification failed");
            cJSON_Delete(j);
        }
    } else
        out = failure("Unknown account action");
    xSemaphoreGive(mutex);
    return out;
}
