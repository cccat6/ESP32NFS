# API guide

`SocketTransport` provides ESP32/ESP-IDF TCP sockets; `PicoWiFiTransport` provides Arduino-Pico Wi-Fi TCP connections. Both implement `Transport`.

All library types live in `esp32_nfs`. Include `ESP32NFS.h` and the headers for your chosen storage and transport.

## Server lifecycle

```cpp
SocketTransport transport;
esp32_nfs::Server nfs(filesystem);
Config config;
config.bytesPerSecond = 0;
config.ioBytesPerPoll = 8192;
uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
nfs.begin(transport, config, epoch ? epoch : 1);
```

Supply a fresh nonzero epoch when starting a server session. Call `nfs.poll(millis())` regularly from the application task. `nfs.stop()` closes its connections and releases session resources. The application owns the transport, filesystem and physical storage objects throughout the server's lifetime.

## Export mapping

```cpp
ExportMap exports;
exports.add("/data", dataFilesystem);
exports.add("/assets", assetFilesystem, true);
esp32_nfs::Server nfs(exports);
```

`add(path, vfs, readOnly, lease)` associates a mount path with a filesystem, an access policy and an optional shared-resource lease. Paths are mounted individually by NFS clients. Mapping and backend generations scope file handles and duplicate-request state.

Stop the server before changing the physical media or remounting a filesystem. Release its caches, mount the new medium, update the export mapping and start a fresh session. `PathVfs::invalidateAll()` refreshes path identities; `notifyDirectoryChanged()` refreshes directory cookies and metadata caches. Use `invalidateAll()` for external namespace replacement, renaming or remounting.

[Names and identities](names-and-identities.md) describes `resolvePath()`, `canonicalPath()` and alias-aware authorization.

## Configuration

| Field | Purpose |
|---|---|
| `exportPath` | Mount path for the single-filesystem constructor |
| `port` | TCP listening port, default 2049 |
| `readOnly` | Read-only policy for a single-filesystem export |
| `portmapper` | PMAP v2 dispatch |
| `blockSize` | Advertised read/write payload size |
| `bytesPerSecond` | Wire bandwidth budget; 0 selects unrestricted throughput |
| `ioBytesPerPoll` | Network byte budget for a poll call |
| `directoryEntriesPerRpc` | Entries returned per directory request |
| `connections` | Number of TCP connection slots |
| `idleTimeoutMs` | Idle-connection lifetime |
| `recordTimeoutMs` | RPC record processing deadline |
| `replayWindowMs` | Duplicate-request retention window |

## Compile-time sizing

| Definition | Default | Resource |
|---|---:|---|
| `ESP32_NFS_BLOCK_SIZE` | 4096 | Maximum NFS data payload |
| `ESP32_NFS_HANDLE_COUNT` | 16 | Path identity pool |
| `ESP32_NFS_CONNECTION_COUNT` | 2 | TCP connection slots |
| `ESP32_NFS_REPLAY_COUNT` | 4 | Mutation replay records |

Apply definitions consistently to the sketch and library translation units. The [performance guide](performance.md) shows transfer and scheduling configurations.

## Shared resources

Implement `Lease::acquire()` and `Lease::release()` to coordinate access to a shared bus or volume. An acquire result of `false` lets the server return a retry response. `storage::LeasedDevice` coordinates individual block transfers, while a volume lease serializes filesystem operations.

## Metrics

`nfs.metrics()` reports received/sent bytes, processed requests, replay hits, rejected connections, timeouts and owned heap usage. Applications can use these counters to choose polling budgets and monitor their service.
