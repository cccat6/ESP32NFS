# Performance configuration

## Polling and bandwidth

Call `poll()` regularly and yield to the ESP32 network and application tasks. Configure bandwidth and per-call work together:

```cpp
Config config;
config.bytesPerSecond = 0;
config.ioBytesPerPoll = 8192;
config.directoryEntriesPerRpc = 16;
```

Use a finite `bytesPerSecond` to allocate a predictable share of network bandwidth. Size the directory batch for the application's directory workload. Names-only browsing uses READDIR; clients requesting file attributes use READDIRPLUS.

## Transfer sizes and client concurrency

The default maximum payload is 4KiB. A throughput-oriented build can set `ESP32_NFS_BLOCK_SIZE=16384` consistently across the sketch and library, then use a 32KiB poll byte budget. Configure the client for matching read/write sizes and pipelined requests.

Larger payloads amortize RPC processing and round-trip time. Choose the request depth together with TCP buffer sizes and the number of connections. Request/reply memory scales with the maximum payload.

## Wi-Fi and TCP

Use `WiFi.setSleep(false)` for continuously powered throughput-oriented applications. `SocketTransport` enables TCP_NODELAY by default and exposes `setNoDelay()` for application-specific TCP behavior.

TCP send capacity and receive windows are SDK build settings. Configure them through a source-built ESP-IDF SDK or Arduino as an ESP-IDF component. A C3 throughput profile can use:

```ini
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=40960
CONFIG_LWIP_TCP_WND_DEFAULT=40960
CONFIG_LWIP_TCP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=20
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=40
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=40
CONFIG_ESP_WIFI_TX_BA_WIN=32
CONFIG_ESP_WIFI_RX_BA_WIN=32
```

Budget these buffers together with filesystem caches, request buffers and application memory. Keep application watchdogs, Flash configuration and task scheduling aligned with the complete firmware design. Espressif documents the available settings in its [Wi-Fi guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/wifi.html).

## Storage access

Use larger sequential operations and batches where the application's data model allows. LittleFS's small-write cache combines scattered updates; COMMIT supplies a checked synchronization point. FILE_SYNC provides a persistence boundary for each request.

FAT/exFAT directory cursors and retained read descriptors reduce repeated filesystem work. Size the path-identity pool for the exported namespace. Coordinate SPI transfers with the application lease so other peripherals receive their scheduled bus access.
