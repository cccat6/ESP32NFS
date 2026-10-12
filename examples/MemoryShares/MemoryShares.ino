// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <WiFi.h>
#include <backends/RamVfs.h>
#include <backends/RomVfs.h>
#include <esp_system.h>
#include <transports/SocketTransport.h>
using namespace esp32_nfs;
const char *WifiSsid = "YOUR_WIFI_SSID";
const char *WifiPassword = "YOUR_WIFI_PASSWORD";
uint8_t memory[4][4096];
RamVfs::Slot slots[4];
RamVfs ram(slots, 4);
const uint8_t welcome[] = "Hello from ESP32NFS!\n";
const RomEntry files[] = {{"/welcome.txt", welcome, sizeof(welcome) - 1}};
RomVfs rom(files, 1);
ExportMap exports;
SocketTransport transport;
esp32_nfs::Server nfs(exports);
void setup() {
  Serial.begin(115200);
  for (unsigned i = 0; i < 4; ++i) {
    slots[i].data = memory[i];
    slots[i].capacity = sizeof(memory[i]);
  }
  exports.add("/ram", ram);
  exports.add("/rom", rom, true);
  WiFi.setSleep(false);
  WiFi.begin(WifiSsid, WifiPassword);
  while (WiFi.status() != WL_CONNECTED)
    delay(50);
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  config.directoryEntriesPerRpc = 16;
  uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
  if (nfs.begin(transport, config, epoch ? epoch : 1))
    Serial.printf("NFS: %s:/ram and /rom\n", WiFi.localIP().toString().c_str());
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
