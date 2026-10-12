// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <SPI.h>
#include <WiFi.h>
#include <backends/SdFatStorage.h>
#include <esp_system.h>
#include <transports/SocketTransport.h>
using namespace esp32_nfs;
#ifndef NFS_EXAMPLE_SSID
#define NFS_EXAMPLE_SSID "YOUR_WIFI_SSID"
#endif
#ifndef NFS_EXAMPLE_PASSWORD
#define NFS_EXAMPLE_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
#ifndef NFS_EXAMPLE_CS
#define NFS_EXAMPLE_CS SS
#endif
// These defaults are intentionally portable. Set Wi-Fi credentials and the board's CS locally.
SdFatCard sd;
SemaphoreHandle_t busMutex;
class BusLease : public Lease {
  bool acquire() override {
    return busMutex && xSemaphoreTake(busMutex, 0) == pdTRUE;
  }
  void release() override {
    xSemaphoreGive(busMutex);
  }
} lease;
SdFatStorage sdStorage(sd);
PathVfs vfs(sdStorage);
SocketTransport network;
esp32_nfs::Server nfs(vfs, &lease);
void setup() {
  Serial.begin(115200);
  if (!strlen(NFS_EXAMPLE_SSID)) {
    Serial.println("Set local Wi-Fi credentials before running.");
    return;
  }
  WiFi.setSleep(false);
  WiFi.begin(NFS_EXAMPLE_SSID, NFS_EXAMPLE_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    delay(50);
  if (WiFi.status() != WL_CONNECTED)
    return;
  // Application owns initialization and arbitration. Never use DEDICATED_SPI on a shared bus.
  busMutex = xSemaphoreCreateMutex();
  if (!busMutex)
    return;
  xSemaphoreTake(busMutex, portMAX_DELAY);
  bool ready = sd.begin(SdSpiConfig(NFS_EXAMPLE_CS, SHARED_SPI, SD_SCK_MHZ(12), &SPI));
  if (ready)
    ready = sdStorage.refreshSpace();
  xSemaphoreGive(busMutex);
  if (!ready)
    return;
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  config.directoryEntriesPerRpc = 16;
  uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
  if (nfs.begin(network, config, epoch ? epoch : 1))
    Serial.printf("NFS TCP %s:%u\n", WiFi.localIP().toString().c_str(), config.port);
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
