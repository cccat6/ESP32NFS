# Storage and filesystems

## Backends

| Backend | Data model | Integration |
|---|---|---|
| `LittleFsStorage` | Persistent internal or external Flash files | Mounted ESP-IDF LittleFS VFS path |
| `SdFatStorage` | FAT32 and exFAT files | Initialized `SdFatCard` or `SdFatVolume` |
| `ArduinoFsStorage` | Read-only Arduino filesystem export | Arduino `fs::FS` |
| `RamVfs` | Volatile memory files | Caller-owned slots and byte buffers |
| `RomVfs` | Read-only firmware files | Static `RomEntry` manifest |

A medium holds bytes; a filesystem organizes those bytes into names, directories and metadata. LittleFS organizes Flash storage. FAT32 and exFAT organize removable block storage. RAM and ROM backends provide file namespaces directly over application memory.

## Metadata and indexing

FAT32 keeps directory entries and a cluster-allocation table. exFAT adds an allocation bitmap and file extent information. LittleFS maintains copy-on-write metadata pairs and its native file structures. These filesystem implementations own persistent names, sizes and timestamps.

`PathVfs` maps paths to runtime file identities. Directory cursors advance through entries, and names-only enumeration serves READDIR. READDIRPLUS obtains each entry's attributes and handle. Backend caches retain file descriptors and metadata between requests; filesystem mutations refresh their associated cached state.

NFS ownership and mode fields follow fixed export/backend policy. FAT's native read-only bit contributes to the displayed access mode. This policy keeps metadata aligned with the storage format. RAM slot records and ROM manifest entries provide the corresponding attributes in memory.

## LittleFS

```cpp
LittleFsStorage flashStorage("/littlefs", "spiffs");
PathVfs files(flashStorage);
```

The application mounts LittleFS before starting the export. The adapter uses native file operations for reads, writes, truncation, synchronization and atomic replacement rename. An 8KiB payload cache combines small UNSTABLE writes into ordered batches. COMMIT and stable WRITE synchronize the file before returning success.

`setSmallWriteCache(bytes)` configures the payload cache; use 0 for direct writes. Power-of-two sizes from 512 through 16384 bytes are accepted. Overlapping patches preserve arrival order, and reads include pending file data.

## SD cards and multiple partitions

UTF-8 long names are enabled automatically. The [name guide](names-and-identities.md) covers the ready-to-use storage objects and canonical file identities.

Initialize a card once, then create a separate `SdFatVolume` and `SdFatStorage` for each mounted partition. Pass a shared `PathVfs::Registry` to account for path identities under one shared memory budget. Use one volume owner and a shared bus lease for coordinated access.

The generic block layer supplies `Partition` views and primary MBR/GPT discovery for application-managed devices. Custom USB, SPI and virtual drivers expose the same block interface described in [driver-interface.md](driver-interface.md).

## Transactional FAT and exFAT

`storage::SectorJournal` provides a sector overlay and recovery log. Provision a dedicated log region outside the filesystem's data range. Device geometry supplies stable media identity, durable flushing and isolated-write semantics.

Integration order:

1. Connect the device to `storage::BlockIo`.
2. Construct `SectorJournal` with the data range and reserved log range.
3. Recover the journal, then mount the filesystem through `SdFatBlockAdapter`.
4. Attach the journal and a filesystem-remount callback with `SdFatStorage::attachTransactions()`.
5. Add the filesystem to `ExportMap` and start the server.

`SdFatVolume::begin()` accepts `SdFatBlockAdapter` directly with standard Arduino build settings.

The journal writes checksummed records and a durable commit marker before applying the transaction to the base filesystem. Recovery replays committed records. This provides transactional replacement rename and EXCLUSIVE CREATE for the SdFat backend.

EXCLUSIVE CREATE stores request verifiers in `/.esp32-nfs/exclusive.bin`. The server manages this private namespace and filters it from exported directory results. Verifiers move with renamed paths and their records are reused after deletion. The lookup index grows in small pages under the shared memory budget and is released when the backend ends its session. Ordinary file operations use the filesystem's native metadata.
