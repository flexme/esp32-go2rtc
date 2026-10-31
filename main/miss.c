// Xiaomi MISS over CS2+TCP. Protocol reference: go2rtc v1.9.14 (MIT).
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sodium.h"
#include "xrtc.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool wanted;
static atomic_uint restart_generation;
static bool camera_wanted(void) { return atomic_load(&wanted); }
static uint64_t millis(void) { return esp_timer_get_time() / 1000; }
static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}
static uint64_t le64(const uint8_t *p) { return ((uint64_t)le32(p + 4) << 32) | le32(p); }
static void put16(uint8_t *p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static void put32(uint8_t *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
typedef struct {
    int fd;
    uint16_t sequence;
    uint64_t last_ping, last_media;
    camera_auth_t auth;
    bool authenticated, streaming;
    uint8_t quality;
    unsigned generation;
    struct {
        uint8_t prefix[4];
        size_t prefix_used, used, expected, capacity;
        uint8_t *data;
    } channels[2];
} connection_t;
static int send_all(int fd, const void *buffer, size_t size) {
    const uint8_t *p = buffer;
    while (size) {
        int n = send(fd, p, size, 0);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        size -= n;
    }
    return 0;
}
static int send_tcp(connection_t *c, const uint8_t *data, size_t length) {
    if (length > 65535)
        return -1;
    uint8_t header[8] = {0};
    put16(header, length);
    header[2] = 0x68;
    return send_all(c->fd, header, 8) || send_all(c->fd, data, length) ? -1 : 0;
}
static int keepalive(connection_t *c) {
    if (millis() - c->last_ping < 900)
        return 0;
    const uint8_t ping[] = {0xf1, 0xe0, 0, 0};
    c->last_ping = millis();
    return send_tcp(c, ping, sizeof(ping));
}
static int read_exact(connection_t *c, uint8_t *data, size_t length) {
    uint64_t deadline = millis() + 12000;
    while (length && camera_wanted() && c->generation==atomic_load(&restart_generation)) {
        if (keepalive(c))
            return -1;
        int n = recv(c->fd, data, length, 0);
        if (n > 0) {
            data += n;
            length -= n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (millis() < deadline)
                continue;
        }
        return -1;
    }
    return length ? -1 : 0;
}
static int command(connection_t *c, uint32_t cmd, const uint8_t *data, size_t size) {
    if (size > 4096)
        return -1;
    uint8_t *packet = malloc(size + 16);
    if (!packet)
        return -1;
    packet[0] = 0xf1;
    packet[1] = 0xd0;
    put16(packet + 2, size + 12);
    packet[4] = 0xd1;
    packet[5] = 0;
    put16(packet + 6, c->sequence++);
    put32(packet + 8, size + 4);
    put32(packet + 12, cmd);
    memcpy(packet + 16, data, size);
    int result = send_tcp(c, packet, size + 16);
    free(packet);
    return result;
}
static int encrypted_command(connection_t *c, uint32_t cmd, const char *json) {
    size_t n = json ? strlen(json) : 0;
    uint8_t *packet = malloc(n + 12);
    if (!packet)
        return -1;
    randombytes_buf(packet, 8);
    put32(packet + 8, cmd);
    if (n)
        memcpy(packet + 12, json, n);
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, packet, 8);
    crypto_stream_chacha20_ietf_xor(packet + 8, packet + 8, n + 4, nonce, c->auth.shared_key);
    int result = command(c, 0x1001, packet, n + 12);
    sodium_memzero(packet, n + 12);
    free(packet);
    return result;
}
static int handshake(const char *ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    struct timeval timeout = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in camera = {.sin_family = AF_INET, .sin_port = htons(32108)};
    inet_aton(ip, &camera.sin_addr);
    uint8_t request[512] = {0xf1, 0x30, 0, 0}, response[512];
    size_t request_size = 4;
    bool tcp = false;
    for (int attempt = 0; attempt < 10 && camera_wanted(); attempt++) {
        if (sendto(fd, request, request_size, 0, (struct sockaddr *)&camera, sizeof(camera)) < 0)
            break;
        struct sockaddr_in source;
        socklen_t slen = sizeof(source);
        int n = recvfrom(fd, response, sizeof(response), 0, (struct sockaddr *)&source, &slen);
        if (n < 16 || source.sin_addr.s_addr != camera.sin_addr.s_addr || response[0] != 0xf1)
            continue;
        if (response[1] == 0x41) {
            camera.sin_port = source.sin_port;
            memcpy(request, response, n);
            request_size = n;
        } else if (response[1] == 0x43) {
            camera.sin_port = source.sin_port;
            tcp = true;
            break;
        }
    }
    close(fd);
    if (!tcp)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int receive_buffer = 64 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    timeout.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    timeout.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&camera, sizeof(camera))) {
        close(fd);
        return -1;
    }
    return fd;
}
static int message(connection_t *c, int channel, uint8_t *data, size_t size) {
    if (channel == 0) {
        if (size < 4)
            return -1;
        if (!c->authenticated) {
            cJSON *j = cJSON_ParseWithLength((char *)data + 4, size - 4);
            bool ok = j && !strcmp(json_string(j, "result"), "success");
            cJSON_Delete(j);
            if (!ok)
                return -1;
            c->authenticated = true;
            xrtc_phase("camera_authenticated", NULL);
            char start[64];
            snprintf(start, sizeof(start), "{\"videoquality\":%u,\"enableaudio\":0}", c->quality);
            if (encrypted_command(c, 0x102, start))
                return -1;
            c->streaming = true;
        }
        return 0;
    }
    if (size < 40)
        return -1;
    uint32_t codec = le32(data + 4);
    if (codec != 5) {
        if (codec == 4)
            xrtc_phase("unsupported_codec", "Camera returned H.264; this MVP expects HEVC");
        return codec == 4 ? -1 : 0;
    }
    int64_t received_us = esp_timer_get_time(); // Complete encrypted frame, before decryption.
    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, data + 32, 8);
    crypto_stream_chacha20_ietf_xor(data + 40, data + 40, size - 40, nonce, c->auth.shared_key);
    c->last_media = millis();
    g_status.camera = true;
    g_status.frames++;
    g_status.video_bytes += size - 40;
    if (strcmp(g_status.phase, "streaming"))
        xrtc_phase("streaming", NULL);
    rtc_video(data + 40, size - 40, le64(data + 16), received_us);
    return 0;
}
static int channel_data(connection_t *c, int index, const uint8_t *data, size_t size) {
    // CS2 length-prefixes a logical command/media message across TCP records.
    while (size) {
        if (c->channels[index].prefix_used < 4) {
            size_t take = 4 - c->channels[index].prefix_used;
            if (take > size)
                take = size;
            memcpy(c->channels[index].prefix + c->channels[index].prefix_used, data, take);
            c->channels[index].prefix_used += take;
            data += take;
            size -= take;
            if (c->channels[index].prefix_used < 4)
                continue;
            c->channels[index].expected = be32(c->channels[index].prefix);
            if (c->channels[index].expected < 4 ||
                c->channels[index].expected > c->channels[index].capacity)
                return -1;
        }
        size_t take = c->channels[index].expected - c->channels[index].used;
        if (take > size)
            take = size;
        memcpy(c->channels[index].data + c->channels[index].used, data, take);
        c->channels[index].used += take;
        data += take;
        size -= take;
        if (c->channels[index].used == c->channels[index].expected) {
            if (message(c, index == 0 ? 0 : 2, c->channels[index].data, c->channels[index].used))
                return -1;
            c->channels[index].used = 0;
            c->channels[index].expected = 0;
            c->channels[index].prefix_used = 0;
        }
    }
    return 0;
}
static void run_session(void) {
    connection_t *c = calloc(1, sizeof(*c));
    if (!c) {
        xrtc_phase("error", "Out of memory");
        return;
    }
    c->fd = -1;
    c->generation=atomic_load(&restart_generation);
    xrtc_config_t cfg;
    config_get(&cfg);
    c->quality = cfg.quality;
    if (!cfg.did[0] || !cfg.camera_ip[0]) {
        xrtc_phase("not_configured", "Select a camera first");
        goto done;
    }
    xrtc_phase("cloud_authorization", NULL);
    esp_err_t auth = cloud_camera_auth(&c->auth);
    if (auth != ESP_OK) {
        xrtc_phase("cloud_error", auth == ESP_ERR_NOT_SUPPORTED
                                      ? "This camera uses a P2P vendor other than CS2"
                                      : "Xiaomi account or camera authorization failed");
        goto done;
    }
    if (!camera_wanted())
        goto done;
    xrtc_phase("camera_handshake", NULL);
    c->fd = handshake(cfg.camera_ip);
    if (c->fd < 0) {
        xrtc_phase("camera_error", "CS2 TCP LAN handshake failed");
        goto done;
    }
    c->channels[0].capacity = 4096;
    c->channels[1].capacity = XRTC_MAX_FRAME + 40;
    c->channels[0].data = malloc(c->channels[0].capacity);
    c->channels[1].data =
        heap_caps_malloc(c->channels[1].capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->channels[0].data || !c->channels[1].data) {
        xrtc_phase("error", "Media buffer allocation failed");
        goto done;
    }
    cJSON *login = cJSON_CreateObject();
    cJSON_AddStringToObject(login, "public_key", c->auth.public_hex);
    cJSON_AddStringToObject(login, "sign", c->auth.sign);
    cJSON_AddStringToObject(login, "uuid", "");
    cJSON_AddNumberToObject(login, "support_encrypt", 0);
    char *text = cJSON_PrintUnformatted(login);
    cJSON_Delete(login);
    int rc = text ? command(c, 0x100, (uint8_t *)text, strlen(text)) : -1;
    free(text);
    if (rc)
        goto done;
    c->last_media = millis();
    uint8_t packet[4096], header[8];
    while (camera_wanted() && c->generation==atomic_load(&restart_generation)) {
        if (read_exact(c, header, sizeof(header)))
            break;
        size_t size = be16(header);
        if (size < 4 || size > sizeof(packet) || header[2] != 0x68 || read_exact(c, packet, size))
            break;
        if (packet[0] != 0xf1)
            break;
        if (packet[1] == 0xd0) {
            if (size < 8 || packet[4] != 0xd1)
                break;
            if (packet[5] == 0 || packet[5] == 2)
                if (channel_data(c, packet[5] == 0 ? 0 : 1, packet + 8, size - 8))
                    break;
        } else if (packet[1] == 0xe0) {
            const uint8_t pong[] = {0xf1, 0xe1, 0, 0};
            if (send_tcp(c, pong, 4))
                break;
        } else if (packet[1] == 0xf1)
            break;
        if (millis() - c->last_media > 15000)
            break;
    }
    if (camera_wanted())
        xrtc_phase("camera_reconnecting", "Camera connection closed or timed out");
done:
    if (c->fd >= 0) {
        if (c->streaming)
            encrypted_command(c, 0x103, NULL);
        shutdown(c->fd, SHUT_RDWR);
        close(c->fd);
    }
    free(c->channels[0].data);
    free(c->channels[1].data);
    sodium_memzero(c, sizeof(*c));
    free(c);
    sodium_memzero(&cfg, sizeof(cfg));
    g_status.camera = false;
    if (!camera_wanted())
        xrtc_phase(g_status.wifi ? "ready" : "wifi_connecting", NULL);
}
static void camera_task(void *arg) {
    for (;;) {
        if (camera_wanted() && g_status.wifi) {
            run_session();
            if (camera_wanted()) {
                g_status.reconnects++;
                for (int i = 0; i < 30 && camera_wanted(); i++)
                    vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else
            vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void miss_set_wanted(bool value) { atomic_store(&wanted, value); }
void miss_restart(void) { atomic_fetch_add(&restart_generation,1); }
void miss_init(void) {
    xTaskCreatePinnedToCore(camera_task, "xiaomi_camera", 24576, NULL, 4, NULL, 1);
}
