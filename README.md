# ESP32NFS

[![CI](https://github.com/cccat6/ESP32NFS/actions/workflows/ci.yml/badge.svg)](https://github.com/cccat6/ESP32NFS/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/cccat6/ESP32NFS)](https://github.com/cccat6/ESP32NFS/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**NFSv3 file sharing for ESP32 and Raspberry Pi Pico W.** Share SD cards, internal Flash, RAM and firmware assets over Wi-Fi through a common storage interface.

[简体中文](README.zh-CN.md) · [Getting started](docs/getting-started.md) · [Storage integration](docs/storage.md) · [API guide](docs/api.md)

## Features

- NFSv3 and MOUNT v3 over TCP, with file access from Linux and macOS.
- UTF-8 names with native filesystem case handling and unified long-name/short-alias identities.
- Multiple named exports backed by separate devices, partitions and filesystems.
- LittleFS, FAT32 and exFAT integration, plus memory-backed RAM and immutable ROM namespaces.
- File and directory operations, random access, buffered writes and checked synchronization.
- Transactional FAT/exFAT replacement and EXCLUSIVE CREATE through the sector journal.
- Standard filesystem, block-device and transport interfaces for custom hardware and virtual storage.
- Configurable transfer sizes, bandwidth, connection counts and cooperative polling.

## Installation

1. Install **esp32 by Espressif Systems** through Arduino Boards Manager.
2. Install **SdFat 2.3.0** through Arduino Library Manager.
3. Download **ESP32NFS-1.1.1.zip** from [Releases](https://github.com/cccat6/ESP32NFS/releases/latest).
4. Choose **Sketch → Include Library → Add .ZIP Library** and select the archive.
5. Open **File → Examples → ESP32NFS → LittleFSShare**.

ESP32NFS uses the Arduino library format. Arduino CLI also supports ZIP installation:

```sh
arduino-cli lib install 'SdFat@2.3.0'
ARDUINO_LIBRARY_ENABLE_UNSAFE_INSTALL=true arduino-cli lib install --zip-path ESP32NFS-1.1.1.zip
```

## Share a filesystem

The application connects Wi-Fi and mounts LittleFS. ESP32NFS maps the filesystem into an NFS export:

```cpp
#include <ESP32NFS.h>
#include <esp_system.h>
#include <backends/LittleFsStorage.h>
#include <transports/SocketTransport.h>

using namespace esp32_nfs;

LittleFsStorage flashStorage("/littlefs", "spiffs");
PathVfs filesystem(flashStorage);
SocketTransport transport;
esp32_nfs::Server nfs(filesystem);

// Call after Wi-Fi and LittleFS initialization.
void startNfs() {
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
  nfs.begin(transport, config, epoch ? epoch : 1);
}

void loop() {
  nfs.poll(millis());
  delay(1);
}
```

The [LittleFSShare example](examples/LittleFSShare/LittleFSShare.ino) provides a complete sketch. Set its Wi-Fi credentials, select the board and upload it. The serial console prints the export address.

Mount it on Linux:

```sh
sudo mkdir -p /mnt/esp32
sudo mount -t nfs -o vers=3,proto=tcp,port=2049,mountport=2049,nolock ESP32_IP:/ /mnt/esp32
```

Mount it on macOS:

```sh
mkdir -p ~/mnt/esp32
mount_nfs -o vers=3,tcp,port=2049,mountport=2049,nolocks ESP32_IP:/ ~/mnt/esp32
```

## Map multiple volumes

Each export has its own filesystem and path:

```cpp
ExportMap exports;
exports.add("/sd", sdFilesystem);
exports.add("/flash", flashFilesystem);
exports.add("/ram", ramFilesystem);
exports.add("/rom", romFilesystem, true);
esp32_nfs::Server nfs(exports);
```

Clients mount `ESP32_IP:/sd`, `ESP32_IP:/flash`, `ESP32_IP:/ram` or `ESP32_IP:/rom` directly. The [MappedVolumes example](examples/MappedVolumes/MappedVolumes.ino) combines two SD partitions with RAM and ROM exports.

## Platforms

CI covers ESP32, S2, S3, C2, C3, C5, C6, C61, H2 and P4, plus Pico W and Pico 2 W. See the [platform matrix](docs/platforms.md) for each build route and its examples.

## Examples

| Example | Application |
|---|---|
| [LittleFSShare](examples/LittleFSShare) | Read and write internal Flash files |
| [SdCardShare](examples/SdCardShare) | Share a FAT32 or exFAT SD card |
| [MappedVolumes](examples/MappedVolumes) | Export two SD partitions, RAM and ROM |
| [MemoryShares](examples/MemoryShares) | Publish RAM storage and firmware assets |
| [PicoWShare](examples/PicoWShare) | Pico Wi-Fi sharing of RAM, ROM and LittleFS assets |
| [PicoSdCardShare](examples/PicoSdCardShare) | Pico Wi-Fi sharing of FAT32 and exFAT SD cards |

## Documentation

- [Getting started](docs/getting-started.md): board setup, filesystem initialization and mounting.
- [Storage integration](docs/storage.md): backends, partitions, metadata and transaction setup.
- [Driver interface](docs/driver-interface.md): integrate USB, SPI and virtual storage drivers.
- [Names and identities](docs/names-and-identities.md): Unicode, canonical paths and file protection.
- [API guide](docs/api.md): exports, server configuration and lifecycle.
- [Performance configuration](docs/performance.md): scheduling, buffers and TCP settings.
- [Release process](docs/releasing.md): GitHub Actions and Arduino Library Manager registration.
- [Contributing](CONTRIBUTING.md): development and pull requests.

## License

ESP32NFS is released under the [MIT License](LICENSE). See [NOTICE.md](NOTICE.md) for dependency attribution.
