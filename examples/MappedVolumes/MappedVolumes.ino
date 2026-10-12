// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <SPI.h>
#include <WiFi.h>
#include <backends/RamVfs.h>
#include <backends/RomVfs.h>
#include <backends/SdFatStorage.h>
#include <esp_system.h>
#include <transports/SocketTransport.h>
#ifndef NFS_EXAMPLE_SSID
#define NFS_EXAMPLE_SSID "YOUR_WIFI_SSID"
#endif
#ifndef NFS_EXAMPLE_PASSWORD
#define NFS_EXAMPLE_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
#ifndef NFS_EXAMPLE_CS
#define NFS_EXAMPLE_CS SS
#endif
using namespace esp32_nfs;
SdFs media; // Owns the physical SD card driver; do not mount a second cache for the same partition.
FsVolume partition1, partition2;
SemaphoreHandle_t busMutex;
class BusLease : public Lease {
  bool acquire() override {
    return busMutex && xSemaphoreTake(busMutex, 0) == pdTRUE;
  }
  void release() override {
    xSemaphoreGive(busMutex);
  }
} sdLease;
SdFatStorage storage1(partition1), storage2(partition2);
PathVfs::Registry sharedIdentities; // A single bounded pool for all path-backed partitions.
PathVfs volume1(storage1, sharedIdentities), volume2(storage2, sharedIdentities);
uint8_t ramData[4][1024];
RamVfs::Slot ramSlots[4];
RamVfs ram(ramSlots, 4);
const uint8_t readme[] = "Immutable firmware content exported from flash.\n";
const RomEntry romFiles[] = {RomEntry("/readme.txt", readme, sizeof(readme) - 1)};
RomVfs rom(romFiles, 1);
ExportMap exports;
SocketTransport tcp;
esp32_nfs::Server nfs(exports);
void setup() {
  Serial.begin(115200);
  if (!strlen(NFS_EXAMPLE_SSID))
    return;
  WiFi.setSleep(false);
  WiFi.begin(NFS_EXAMPLE_SSID, NFS_EXAMPLE_PASSWORD);
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000)
    delay(50);
  if (WiFi.status() != WL_CONNECTED)
    return;
  for (unsigned i = 0; i < 4; ++i) {
    ramSlots[i].data = ramData[i];
    ramSlots[i].capacity = sizeof(ramData[i]);
  }
  exports.add("/ram", ram);
  exports.add("/rom", rom, true);
  busMutex = xSemaphoreCreateMutex();
  if (!busMutex)
    return;
  xSemaphoreTake(busMutex, portMAX_DELAY);
  if (media.cardBegin(SdSpiConfig(NFS_EXAMPLE_CS, SHARED_SPI, SD_SCK_MHZ(12), &SPI))) {
    // Explicit MBR partition numbers. An absent/unsupported filesystem is not exported.
    if (partition1.begin(media.card(), false, 1) && storage1.refreshSpace())
      exports.add("/sd/1", volume1, false, &sdLease);
    if (partition2.begin(media.card(), false, 2) && storage2.refreshSpace())
      exports.add("/sd/2", volume2, false, &sdLease);
  }
  xSemaphoreGive(busMutex);
  Config c;
  c.bytesPerSecond = 0;
  c.ioBytesPerPoll = 8192;
  c.directoryEntriesPerRpc = 16;
  uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
  nfs.begin(tcp, c, epoch ? epoch : 1);
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
