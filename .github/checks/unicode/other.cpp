// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <backends/SdFatStorage.h>
bool otherOpenRoot() {
  esp32_nfs::fat::FsBaseFile file;
  bool opened = file.open("/", O_RDONLY);
  if (opened)
    file.close();
  return opened;
}
