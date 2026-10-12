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

The default payload is 4KiB. Set `config.blockSize = 16384` and `config.ioBytesPerPoll = 32768` to use a 16KiB payload at runtime. Size the shared `Memory` budget for the selected transfer size and namespace workload. Configure the client for matching read/write sizes and pipelined requests.

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

## Large directories

Path identities use hash indexes by ID and by parent/name. Nodes store each filename once and refer to their parent by stable ID. Directory renames update the moved node and retain descendant IDs. Directory reads advance with cookies and bounded batches. Reopening a backend cursor preserves identities; interrupted seeks return `Jukebox` so the client can resume the same cookie.

Identity storage grows on demand within the shared memory budget. Deleted entries are reclaimed immediately, sparse index tables shrink, and ending a session releases its entire index. Live identities remain valid for the session. Runtime budget exhaustion returns a retry error before a CREATE or RENAME changes the filesystem. CI exercises 1100-file FAT32/exFAT directories, earlier handles after a full scan, alias equivalence, directory renames, concurrent budget accounting, allocation failures and repeated stop/start.

Optional filesystem and block adapters are supplied as inline implementation headers. Selecting the public adapter header includes its implementation; RAM/ROM applications compile the protocol core without the FAT, path or block backends. The CI inspects both compilation output and ELF symbols for this separation.
