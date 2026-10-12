# Attribution

ESP32NFS original source is distributed under the MIT License.

| Dependency | Integration | License |
|---|---|---|
| [Arduino ESP32](https://github.com/espressif/arduino-esp32) | ESP32 platform, Wi-Fi and filesystem APIs | LGPL-2.1 and bundled component licenses |
| [Arduino-Pico](https://github.com/earlephilhower/arduino-pico) | Pico W and Pico 2 W networking and filesystem APIs | LGPL-2.1 and bundled component licenses |
| [SdFat](https://github.com/greiman/SdFat) 2.3.0 | FAT32 and exFAT | MIT |
| [LittleFS](https://github.com/littlefs-project/littlefs) | Platform-provided Flash filesystem | BSD-3-Clause |
| ArduinoSATA | Optional block-device adapter | BSD-2-Clause |

The protocol implementation follows [RFC 1813](https://www.rfc-editor.org/rfc/rfc1813) and [RFC 5531](https://www.rfc-editor.org/rfc/rfc5531). The original SHA-256 implementation follows FIPS 180-4.

Dependencies are installed through the platform and library toolchains. Preserve their supplied notices when distributing linked firmware.

CI uses [libnfs 8.0.0](https://github.com/sahlberg/libnfs) as an independent LGPL-2.1 client, fetched into the runner workspace.
