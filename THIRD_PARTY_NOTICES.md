# Third-party notices


- Xiaomi account, MISS/CS2 and media protocol implementation is a C adaptation of AlexxIT/go2rtc v1.9.14, commit `b5948cfb25404cc5cb37b166ecaa2dca20b11d4b`. The original MIT license is preserved in `third_party/go2rtc-LICENSE`. No go2rtc process or Go runtime is required by the firmware.
- `third_party/libpeer/` vendors the C sources, license and README of sepfy/libpeer commit `5b849de378545c31d34759a145413846953e1366`, MIT. Local changes add HEVC SDP and an RTP send entry point; use the active default interface; bound SDP, ICE and RTCP input; discover peer-reflexive ICE candidates only from authenticated STUN requests; and adapt Mbed TLS 4 initialization. Offers use the DTLS client role for browser compatibility. Peer certificates are retained and checked against the SDP SHA-256 fingerprint before admitting media; failed handshakes terminate and partially created SRTP contexts are freed. Data channels, remote media reception and generic MQTT/HTTP signaling are not used by this MVP. The original project examples and submodules are intentionally not vendored.
- `espressif/libsodium`, `espressif/cjson`, `sepfy/srtp` are fetched by ESP-IDF Component Manager at the versions/hashes recorded in `dependencies.lock`. Their licenses remain in the downloaded components. The SRTP dependency's 32-bit integer pointer compatibility flags are scoped to that dependency for IDF 6 / GCC 15.
- ESP-IDF is required to build. Its components and bundled TLS stack retain their upstream licenses.

The local ESP RTP adapter yields one 1 kHz scheduler tick after each group of four outgoing datagrams. The UDP adapter retries transient Wi-Fi TX buffer exhaustion at most twenty times, with a 10 ms wait per retry. The custom RTP entry point propagates encryption/send errors to the HEVC keyframe gate and drop counter. The local pacing is bounded backpressure, not a congestion-control implementation.

The application permits up to 500 ms of TCP receive backpressure while the peer mutex is busy. If that bound is exceeded, the next acquired media frame re-enters the keyframe gate instead of forwarding dependent frames after a local drop.

This project is independent of Xiaomi, Espressif, go2rtc and libpeer. Brand names identify compatible products. Xiaomi's cloud and camera protocols may change. This MVP supports the MISS protocol with CS2 TCP transport; TUTK/legacy cameras are not implemented.

The new MIT `components/peer/rtp_repair.*` module maintains a bounded 512-packet cache of already protected RTP datagrams. NACK feedback is parsed according to [RFC 4585 section 6.2.1](https://www.rfc-editor.org/rfc/rfc4585#section-6.2.1). Only successfully authenticated SRTCP reaches the parser; cached ciphertext/tags are resent unchanged, without re-encrypting a repeated sequence. SSRC, FCI lengths, padding, per-feedback work and cache bounds are checked. SRTP/SRTCP authentication failure is now rejected before incoming media/feedback processing. This implements same-SSRC packet repair, not a separate RTX SSRC or full congestion control.

HD negotiation advertises HEVC Main / level 5.1 (`level-id=153`) to accommodate the reference 2960×1666 stream. The optional `main/metrics.*` and `main/www/diagnostics.js` code records only timing/counter metadata and uses no third-party scripts.

