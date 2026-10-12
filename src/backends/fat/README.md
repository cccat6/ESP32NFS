# SdFat filesystem core

This directory contains the runtime FAT/exFAT filesystem portion of [SdFat 2.3.0](https://github.com/greiman/SdFat/tree/2.3.0), by Bill Greiman, under its MIT license. Each source file preserves the upstream copyright and license.

ESP32NFS builds these classes in `esp32_nfs::fat`, with private header guards, UTF-8 long filenames enabled, and a generic block-device interface. Card transports, character conversion, the upcase table, date callbacks, formatting helpers and placement allocation come from the installed SdFat dependency. Their symbols and implementations are shared. Optional inline implementations share volume state across translation units, and unmount clears the selected volume. The filesystem classes and volume state have separate symbols, so a sketch can use ordinary SdFat alongside ESP32NFS.

The FAT name scanner retains a validated LFN chain when opening its short alias. This gives both spellings the stored long name in one directory scan. Damaged LFN checksums propagate a read error through lookup and enumeration. Generic block I/O uses a private cache implementation to keep its device-pointer type consistent across translation units. Filename configuration is fixed in this copy; application compiler definitions configure the installed SdFat dependency independently.

Volume operations use the core file classes, keeping Arduino Stream wrappers out of the NFS storage path.

The public entry points are `SdFatCard`, `SdFatVolume` and `SdFatStorage`. CI exercises both filesystem implementations in one executable, FAT32/exFAT Unicode names, short aliases, transaction recovery and independent NFS clients. Unreferenced filesystem code is removed by the target linker.

Private formatter and diagnostic-dump copies are omitted. Applications can use the installed SdFat formatting API under their device lease. Hash and directory-checksum rotations explicitly use unsigned arithmetic.
