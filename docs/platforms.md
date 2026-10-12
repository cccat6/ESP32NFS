# Platforms and CI

ESP32NFS serves ESP32 applications and Raspberry Pi Pico Wi-Fi boards through a shared NFS core and storage interfaces.

| Platform | Build route | CI coverage |
|---|---|---|
| ESP32, ESP32-S2, ESP32-S3 | Arduino ESP32 3.3.11 | LittleFS, SD, mapped volumes and memory examples |
| ESP32-C3, ESP32-C5, ESP32-C6 | Arduino ESP32 3.3.11 | LittleFS, SD, mapped volumes and memory examples |
| ESP32-H2, ESP32-P4 | Arduino ESP32 3.3.11 | TCP transport and filesystem integration |
| ESP32-C2, ESP32-C61 | ESP-IDF 5.5.4 source build | NFS core, TCP transport, path VFS and block-storage interfaces |
| Raspberry Pi Pico W | Arduino-Pico 6.0.0 | PicoWShare and PicoSdCardShare |
| Raspberry Pi Pico 2 W | Arduino-Pico 6.0.0 | PicoWShare and PicoSdCardShare |

The matrix covers all ten SoCs declared by Arduino ESP32 3.3.11, plus the RP2040 and RP2350 Wi-Fi boards. Each GitHub Release runs the complete matrix and the UTF-8/file-identity regression job. That job runs real SdFat image filesystems with an independent libnfs client on Linux. Default compiler settings apply throughout. ESP32-C3, Pico W and Pico 2 W also link ordinary SdFat and ESP32NFS storage in one application. Generated build applications and compiler output live in the runner's temporary workspace.

## ESP32 networking

`SocketTransport` integrates with lwIP on ESP32 and ESP-IDF. Applications initialize their Wi-Fi, Ethernet or external network interface before starting NFS. The H2/P4 build route checks the shared TCP and storage interfaces independently of a board's network-controller selection.

ESP32-C2 and ESP32-C61 use the source SDK route in CI. Espressif describes their build integration in its [supported-chip documentation](https://docs.espressif.com/projects/arduino-esp32/en/latest/getting_started.html).

## Pico networking

`PicoWiFiTransport` integrates with Arduino-Pico's `WiFiServer` and `WiFiClient`. It owns a fixed connection pool, limits each write to the client's advertised transmit capacity and uses asynchronous copied writes. Its public controls include TCP_NODELAY and optional portmapper discovery.

`PicoWShare` exports RAM, ROM and an initialized LittleFS volume for read-only file access. `PicoSdCardShare` provides writable FAT32/exFAT storage through SdFat. Both examples compile for Pico W and Pico 2 W.

Install **Raspberry Pi Pico/RP2040/RP2350 by Earle F. Philhower, III** through Boards Manager using its [package index](https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json). Select Pico W or Pico 2 W, choose a filesystem partition when using LittleFS, set Wi-Fi credentials and open the corresponding library example.
