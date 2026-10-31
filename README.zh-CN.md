# esp32-go2rtc

运行在 ESP32-S3 上的独立摄像头 WebRTC 桥接程序，使用 C 和 ESP-IDF，受 [go2rtc](https://github.com/AlexxIT/go2rtc) 启发。

当前实现 **小米米家 MISS/CS2 TCP → H.265 WebRTC**。它覆盖 go2rtc 的一部分流媒体用途，并非 Go 程序移植，也不具备 go2rtc 全部协议。

ESP32 自行完成小米云会话授权、局域网私有协议连接、解密及 RTP/ICE/DTLS/SRTP 转发。视频保持压缩编码，不在板端解码或转码，不需要电脑中转。项目只包含通用流媒体功能。

## 当前能力

- 网页配置 Wi-Fi、米家地区/账号和摄像头，本机 NVS 保存配置。
- 账号密码、验证码/验证流程，以及已有账号 passToken 导入；不是摄像头 miIO token。
- 单账号、单摄像头、单 WebRTC 观看者；SD/HD 由摄像头选择。
- H.265 参数集缓存、关键帧启动、RFC7798 分包、有界 NACK 重传。
- 管理/信令接口认证、观看租约、可选性能诊断。

当前没有 RTSP、ONVIF、音频、对讲、多路/多人观看、TURN、公网信令、OTA 或转码。仅支持使用 MISS/CS2 TCP 的兼容型号，不支持 TUTK/旧协议设备。浏览器及操作系统必须支持 H.265 WebRTC。

参考硬件为 ESP32-S3、16 MB Flash / 8 MB 八线 PSRAM，2.4 GHz Wi-Fi，无需板载显示屏、扬声器或摄像头模组。当前适配测试涉及 chuangmi.camera.069a01 的标清与高清；不保证所有小米型号兼容。默认推荐先使用标清；高清大帧可能卡顿。账号验证流程并非每种情况都经过实机验收，这是实验性项目。

## 构建和使用

使用 ESP-IDF 6.0.1：

```sh
git clone https://github.com/flexme/esp32-go2rtc.git
cd esp32-go2rtc
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT -b 230400 flash monitor
```

先备份板子原固件，并核对 Flash、PSRAM 和分区表。当前分区为单个 6 MB factory 应用，地址 0x20000；NVS 为 0x9000，没有 OTA 分区。全量刷写会替换旧固件及分区布局，不需要烧写 eFuse。

串口会显示管理 token 和初始接入点密码。连接 xiaomi-rtc-… 热点后，打开其网关地址配置 Wi-Fi；随后使用板子的 DHCP 地址访问。输入管理 token，选择米家地区并登录，载入账号摄像头，核对局域网地址后保存，然后点击 Play · WebRTC。

初始固件不含任何账号或网络配置。视频走局域网，但新建会话仍需小米云授权，不承诺完全离线。管理页面使用 HTTP，只应在可信局域网访问，不要映射到公网。NVS 未由本项目启用 Flash 加密，应保护串口和实体设备。

API、测试方法、限制及授权说明见 [英文 README](README.md)。原始应用 MIT，上游许可及来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
