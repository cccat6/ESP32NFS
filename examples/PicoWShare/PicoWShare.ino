// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <backends/ArduinoFsStorage.h>
#include <backends/RamVfs.h>
#include <backends/RomVfs.h>
#include <pico/rand.h>
#include <transports/PicoWiFiTransport.h>
using namespace esp32_nfs;
const char *WifiSsid = "YOUR_WIFI_SSID";
const char *WifiPassword = "YOUR_WIFI_PASSWORD";
uint8_t memory[4][4096];
RamVfs::Slot slots[4];
RamVfs ram(slots, 4);
const uint8_t welcome[] = "Hello from ESP32NFS on Pico W!\n";
const RomEntry assets[] = {{"/welcome.txt", welcome, sizeof(welcome) - 1}};
RomVfs rom(assets, 1);
ArduinoFsStorage flashStorage(LittleFS);
PathVfs flashFiles(flashStorage);
ExportMap exports;
PicoWiFiTransport transport;
esp32_nfs::Server nfs(exports);
void setup() {
  Serial.begin(115200);
  for (unsigned i = 0; i < 4; ++i) {
    slots[i].data = memory[i];
    slots[i].capacity = sizeof(memory[i]);
  }
  exports.add("/ram", ram);
  exports.add("/rom", rom, true);
  LittleFS.setConfig(LittleFSConfig(false));
  if (LittleFS.begin())
    exports.add("/flash", flashFiles, true);
  WiFi.begin(WifiSsid, WifiPassword);
  while (WiFi.status() != WL_CONNECTED)
    delay(50);
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  config.directoryEntriesPerRpc = 16;
  uint64_t epoch = (uint64_t(get_rand_32()) << 32) | get_rand_32();
  if (nfs.begin(transport, config, epoch ? epoch : 1))
    Serial.printf("NFS: %s:/ram and /rom\n", WiFi.localIP().toString().c_str());
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
