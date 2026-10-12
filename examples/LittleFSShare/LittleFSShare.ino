// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <backends/LittleFsStorage.h>
#include <esp_system.h>
#include <transports/SocketTransport.h>
using namespace esp32_nfs;
#ifndef NFS_EXAMPLE_SSID
#define NFS_EXAMPLE_SSID "YOUR_WIFI_SSID"
#endif
#ifndef NFS_EXAMPLE_PASSWORD
#define NFS_EXAMPLE_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
constexpr bool FormatOnMountFailure = false; // Set true to initialize a new filesystem.
LittleFsStorage flashStorage("/littlefs", "spiffs");
PathVfs vfs(flashStorage);
SocketTransport network;
esp32_nfs::Server nfs(vfs);
void setup() {
  Serial.begin(115200);
  if (!strlen(NFS_EXAMPLE_SSID))
    return;
  WiFi.setSleep(false);
  WiFi.begin(NFS_EXAMPLE_SSID, NFS_EXAMPLE_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000)
    delay(50);
  if (WiFi.status() != WL_CONNECTED || !LittleFS.begin(FormatOnMountFailure))
    return;
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  config.directoryEntriesPerRpc = 16;
  uint64_t epoch = (uint64_t(esp_random()) << 32) | esp_random();
  if (nfs.begin(network, config, epoch ? epoch : 1))
    Serial.printf("NFS: %s:/\n", WiFi.localIP().toString().c_str());
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
