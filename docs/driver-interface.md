# Storage driver interface

ESP32NFS connects storage through three layers:

```text
Device driver → BlockDevice → Partition → Filesystem → Vfs → ExportMap → NFS
```

Applications can also supply a `Vfs` directly for memory-backed or virtual files.

## BlockDevice

Implement `storage::BlockDevice` for a controller or storage service:

```cpp
class Device : public esp32_nfs::storage::BlockDevice {
public:
  esp32_nfs::storage::Geometry geometry() const override;
  esp32_nfs::storage::Result start(const esp32_nfs::storage::Request &) override;
  esp32_nfs::storage::Result poll() override;
  void cancel() override;
};
```

`Geometry` describes block count, block size, transfer alignment, maximum transfer size, media identity, runtime generation and write/flush capabilities. Keep media identity stable across restarts. Change generation when the attached medium changes.

`Request` selects Read, Write or Flush and carries the target LBA, block count, buffer and generation. Return the actual completed block count with successful I/O.

| Result | Driver state |
|---|---|
| `Ok` | Operation completed |
| `Busy` | Request awaits admission; the caller may retry |
| `Pending` | Request admitted; completion proceeds through `poll()` |
| `Changed` | Caller should refresh the medium and filesystem mapping |
| `NoMedia` | Device is waiting for media |
| `RecoveryRequired` | Device needs recovery before resuming service |

The driver borrows request buffers until completion or cancellation. `cancel()` synchronously releases DMA and buffer access. A successful durable Flush covers previously acknowledged writes and controller caches. Advertise each geometry capability according to the implemented driver contract.

`storage::CallbackDevice` adapts the equivalent function table for C-style drivers. `BlockIo` provides a filesystem-facing interface with a monotonic clock and an application-supplied wait/yield callback. `LeasedDevice` holds a shared bus during an admitted transfer and releases it on completion.

## Partition mapping

`Partition(parent, firstBlock, blockCount, readOnly)` creates a checked range view. Applications may define ranges directly or use `discoverPartitions()` with aligned scratch buffers to read primary MBR and GPT entries. Partition discovery validates metadata, CRCs, ranges and overlaps.

For Flash devices, the block driver supplies the erase, translation, cache and wear-management behavior appropriate to the medium. A filesystem then consumes its logical block interface.

## Filesystem integration

Implement `PathStorage` to adapt a filesystem with paths. `PathVfs` supplies NFS file identities and maps operations onto that backend. Its operations cover attributes, reads, writes, creation, removal, replacement rename, directory entries, synchronization and capacity.

Implement `Vfs` directly for generated files, RAM data, ROM manifests or application-owned namespaces. `Vfs::generation()` represents the backend lifetime and participates in export handle validity.

A write-back backend advertises `bufferedWrites()` and copies accepted input through `writeBuffered()`. `sync()` establishes the persistence boundary used by stable WRITE and COMMIT. `entryInfo()` lets a filesystem return attributes acquired during enumeration; `entry()` supplies names for lightweight directory browsing.

## Lifecycle

The application owns device initialization, physical bus configuration and filesystem mounting. During a media change, stop exports, release backend caches, refresh the driver and filesystem, update mappings and start a new server session. Shared volume ownership coordinates local file access with NFS operations.
