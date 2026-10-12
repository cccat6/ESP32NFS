# Getting started

## Board and dependencies

Install the Espressif ESP32 Arduino core and SdFat 2.3.0. The build workflow uses ESP32 core 3.3.11 and covers the complete ESP32 SoC family through its Arduino and source-SDK routes. Arduino-Pico 6.0.0 supplies Pico W and Pico 2 W integration; see the [platform guide](platforms.md).

Select your board and its Flash size in Arduino IDE. For LittleFS, choose a partition scheme containing a filesystem partition. The LittleFSShare example uses the Arduino `spiffs` partition label and the `/littlefs` VFS mount path.

## Internal Flash

Open `LittleFSShare`, set `NFS_EXAMPLE_SSID` and `NFS_EXAMPLE_PASSWORD`, and upload the sketch. Set `FormatOnMountFailure` to `true` when initializing an empty filesystem partition. Once initialized, use `false` for subsequent starts.

The application mounts LittleFS, creates a `PathVfs`, and starts the TCP server. Open Serial Monitor at 115200 baud to read the export address.

## SD cards

The ESP32 SD examples supply UTF-8/LFN options through `build_opt.h`. Apply the [same global build definitions](names-and-identities.md) when integrating the library into an application or Pico sketch.

Open `SdCardShare` and set the Wi-Fi credentials and `NFS_EXAMPLE_CS`. Configure your board's SPI pins before card initialization when using a custom wiring layout. The example uses shared SPI with a FreeRTOS mutex, so the application can coordinate storage with other peripherals.

FAT32 provides broad removable-media compatibility. exFAT provides large-file storage. `SdFatStorage` connects either filesystem to the same NFS interface.

`MappedVolumes` mounts primary SD partitions 1 and 2 and exports them as `/sd/1` and `/sd/2`. Each partition owns a filesystem instance and shares the card's bus lease.

## RAM and ROM

`MemoryShares` exports `/ram` and `/rom`. Each RAM slot has an application-owned byte buffer and capacity. RAM data lives for the current firmware session, and NFS writes use UNSTABLE semantics. ROM exposes a static file manifest as a read-only namespace. Mount `/rom` to read `welcome.txt` immediately.

## Client mounting

Replace `ESP32_IP` with the address printed by the sketch. The single-volume examples export `/`.

Linux:

```sh
sudo mkdir -p /mnt/esp32
sudo mount -t nfs -o vers=3,proto=tcp,port=2049,mountport=2049,nolock ESP32_IP:/ /mnt/esp32
ls /mnt/esp32
sudo umount /mnt/esp32
```

macOS:

```sh
mkdir -p ~/mnt/esp32
mount_nfs -o vers=3,tcp,port=2049,mountport=2049,nolocks ESP32_IP:/ ~/mnt/esp32
ls ~/mnt/esp32
umount ~/mnt/esp32
```

For ROM, add `ro` to the mount options and use `ESP32_IP:/rom`. Use a read-only mount for browsing the volatile RAM example with an operating-system client. NFS applications can write RAM files with UNSTABLE requests.

The server uses TCP port 2049 for both NFS and MOUNT. Explicit port options connect directly to the service. Applications can enable TCP portmapper discovery through `SocketTransport::enableDiscovery(true)` and `Config::portmapper`.
