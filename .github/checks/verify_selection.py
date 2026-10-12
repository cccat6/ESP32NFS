#!/usr/bin/env python3
import pathlib, subprocess, sys
build = pathlib.Path(sys.argv[1])
libraries = build / "libraries"
assert not (libraries / "SdFat").exists(), "RAM/ROM build compiled SdFat"
elf = next(build.glob("*.elf"))
# binutils path is supplied by the target's installed toolchain.
symbols = subprocess.check_output([sys.argv[2], "-C", str(elf)], text=True)
for name in ("esp32_nfs::fat::", "esp32_nfs::PathVfs", "SdFatStorage", "SectorJournal", "discoverPartitions"):
    assert name not in symbols, "Unselected backend linked: " + name
print("RAM/ROM: no SdFat compilation and no optional filesystem/block symbols")
