# esp32-go2rtc

A small, standalone **ESP32-S3 camera-to-WebRTC bridge**, inspired by [go2rtc](https://github.com/AlexxIT/go2rtc). Written in C with ESP-IDF.

**Current implementation: Xiaomi Mi Home MISS/CS2 TCP → H.265 WebRTC.** This is an experimental subset of go2rtc's streaming scope, not a port of its Go runtime or a drop-in replacement for its complete protocol catalog.

[中文说明](README.zh-CN.md)

```text
Mi Home cloud ── session authorization ── ESP32-S3
Xiaomi camera ── encrypted LAN video ── ESP32-S3 ── ICE / DTLS / SRTP ── Browser
```

The ESP32 handles cloud authorization, private camera transport, media decryption and RTP packetization. Compressed video is forwarded without decoding or transcoding. No computer relay is needed.

## Features

- Web-based Wi-Fi, Xiaomi account and camera configuration, stored locally in NVS.
- Account password/captcha/verification flows and optional existing account passToken import.
- Camera discovery within the account; configurable region, model and LAN IPv4 address.
- Camera-side SD or HD selection; H.265 passthrough with parameter-set caching and keyframe startup.
- RFC 7798 packetization, ICE, DTLS certificate-fingerprint checks, SRTP and bounded RTCP NACK repair.
- Authenticated management/signaling API, single-viewer leases and optional performance diagnostics.

## Scope and limitations

One account, one camera, one LAN viewer, video only. Implemented transport is **MISS with CS2 TCP**; legacy Xiaomi/TUTK cameras are not supported. RTSP serving/input, ONVIF, audio, talkback, multiple viewers, TURN, Internet exposure, OTA and transcoding are not implemented. Hardware peripherals and computer-side integrations are outside this repository.

A compatible H.265 WebRTC browser/OS is required. SD 848×480 and HD 2960×1666 have been exercised with `chuangmi.camera.069a01`; this does not imply all Xiaomi models work. HD can show stalls on large frames even when packet loss is zero. SD is the default. Password and verification UI paths are implemented, but not all account challenge variants have hardware validation. Treat this as a prototype, not production surveillance infrastructure.

## Hardware and build

Reference target: ESP32-S3, 16 MB flash and 8 MB octal PSRAM, 2.4 GHz Wi-Fi. PSRAM is required. No display, speaker or camera sensor wired to the ESP32 is required.

Use ESP-IDF **6.0.1** and a clean checkout:

```sh
git clone https://github.com/flexme/esp32-go2rtc.git
cd esp32-go2rtc
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT -b 230400 flash monitor
```

Replace `PORT` with your serial device. Review flash/PSRAM settings for your board before flashing. The supplied partition table uses a 6 MB factory app at `0x20000`, with NVS at `0x9000`, and no OTA partition. A full flash overwrites the existing firmware/partition layout; back up the board first. No eFuse programming is needed. The built application is `build/esp32_go2rtc.bin`.

## Configure and play

1. Read the setup token and AP credentials from the physical USB serial console.
2. Join the board's `xiaomi-rtc-…` access point and open its gateway address. Configure the 2.4 GHz Wi-Fi connection, then reopen the UI at the board's DHCP address.
3. Enter the setup token. Select the Mi Home region, then sign in or import an account **passToken** (not the camera's miIO token).
4. Load cameras, select a compatible device, verify its LAN IPv4 address, and save.
5. Select SD initially and click **Play · WebRTC**. Stop playback before changing camera/account settings.

Media travels over the LAN, but new camera sessions still require Xiaomi cloud authorization. Fully offline operation is not promised. Configuration is deliberately blank in a fresh build; no account or network is embedded in firmware.

## API

Send `Authorization: Bearer <setup-token>` to every `/api/` endpoint. Requests and responses are JSON unless noted. The default server is HTTP on port 80; use only on a trusted LAN. Do not expose it to the Internet. NVS credentials are not flash-encrypted by this project; protect physical access and serial logs.

| Endpoint | Method | Purpose |
| --- | --- | --- |
| `/api/status` | GET | Connection and media counters |
| `/api/config` | GET / POST | Public settings / update settings |
| `/api/account` | POST | Login or complete an account challenge |
| `/api/devices` | GET | List account cameras |
| `/api/time` | POST | Set UTC: `{"epoch":<unix-seconds>}` |
| `/api/clock` | GET | Board monotonic clock |
| `/api/rtc/offer` | POST | Create the single viewer; returns `session` and `sdp` |
| `/api/rtc/answer` | POST | Submit `{"session":...,"sdp":"..."}` |
| `/api/rtc/heartbeat` | POST | Renew `{"session":...}` |
| `/api/rtc/stop` | POST | Stop that session; empty `{}` is an administrator global stop |
| `/api/rtc/timings` | GET | Optional frame timing snapshot |
| `/api/restart` | POST | Restart the board |

An unanswered heartbeat lease expires after 20 seconds. The bundled player demonstrates SDP negotiation and two-second heartbeats. Session-scoped stops cannot evict a later viewer. Cloud TLS verification and DTLS fingerprint verification are enabled.

## Tests

```sh
sh tests/run_host.sh
python3 tests/test_public_scope.py
```

Host tests exercise HEVC packetization/keyframe gates, RTP wraparound, authenticated repair budgets and browser diagnostic calculations. The publication-scope check rejects excluded modules and common private-file patterns. Hardware streaming tests require a separately configured board and compatible camera; a successful build alone does not prove compatibility.

## License and attribution

Original application code: MIT. Protocol implementation is adapted from AlexxIT/go2rtc; the WebRTC core vendors sepfy/libpeer with local H.265/ESP-IDF adaptations. Preserve upstream notices when redistributing. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the vendored license files.

Independent project; not affiliated with Xiaomi, Espressif or the go2rtc authors. Contributions for streaming protocols and ESP32 reliability are welcome. Never put credentials, private recordings or device backups in issues or pull requests.
