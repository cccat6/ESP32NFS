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

The default service provides two TCP slots, four export mappings and four mutation-replay records. Payload size, bandwidth, polling work and memory budgets use runtime configuration. The [performance guide](performance.md) describes these settings.

## Shared resources

Implement `Lease::acquire()` and `Lease::release()` to coordinate access to a shared bus or volume. An acquire result of `false` lets the server return a retry response. `storage::LeasedDevice` coordinates individual block transfers, while a volume lease serializes filesystem operations.

## Metrics

`nfs.metrics()` reports received/sent bytes, processed requests, replay hits, rejected connections, timeouts and owned heap usage. Applications can use these counters to choose polling budgets and monitor their service.

## Runtime memory and lifecycle

`Memory` owns the allocation budget shared by default across NFS work buffers, path identities, LittleFS write caches and EXCLUSIVE verifier pages. Its defaults are a 128KiB charged-allocation budget and a 32KiB free-heap reserve check. These are adjustable runtime policy values. `MemoryLimits` applies immediately when `configure()` succeeds; rejection preserves the prior limits. `getLimits()` returns the applied policy. Configuration and allocation admission use a nonblocking guard, and allocation counters are atomic.

```cpp
MemoryLimits limits;
limits.maxBytes = 192 * 1024;
limits.reserveBytes = 32 * 1024;
bool applied = defaultMemory().configure(limits);
MemoryStats memory = defaultMemory().stats();
```

A caller-owned `Memory` can select another allocator and can be shared explicitly by `PathVfs::Registry`, `Server`, `SdFatStorage` and `LittleFsStorage`. Keep it alive for all its consumers. Individual filesystem and server operations remain serialized by their owner and volume/bus leases.

`MemoryStats` reports charged bytes, peak bytes, allocation count, allocation failures and the last resource error: budget, reserve, allocator failure, invalid request or temporary admission contention. Charging includes a 16-byte bookkeeping allowance per allocation. `Jukebox` is the NFS retry result for allocation pressure. Transport workspace failures also update `Server::lastStatus()` and `metrics().memoryFailures`.

`Server::stop()` returns a `Status`, closes network activity, releases protocol buffers, and ends export sessions under their leases. A busy lease returns `Jukebox`; retry `stop()` after the owner releases it. Synchronization errors are reported as errors. `PathVfs::endSession()` releases its identities, while `releaseCaches()` frees backend caches and preserves live identities. `Server::unmount(path)` removes one export under the same lease. Start a new session with `begin()` and a fresh epoch. The application owns its RAM file data and physical volume mount lifecycle.

`Config::blockSize` selects payloads from 512 through 32768 bytes at runtime. The default is 4096. Request/reply buffers grow with this value; `Server::activeWorkspaceBytes(payload)` reports their payload accounting. Default polling allows 8192 network bytes and up to 16 directory entries per RPC, bounded further by the client's reply size. `bytesPerSecond=0` uses the available link capacity; a positive value enables the runtime bandwidth policy.
