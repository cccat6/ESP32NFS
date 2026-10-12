// SPDX-License-Identifier: MIT
#include <ESP32NFS.h>
#include <SPI.h>
#include <WiFi.h>
#include <backends/SdFatStorage.h>
#include <pico/rand.h>
#include <transports/PicoWiFiTransport.h>
using namespace esp32_nfs;
const char *WifiSsid = "YOUR_WIFI_SSID";
const char *WifiPassword = "YOUR_WIFI_PASSWORD";
constexpr uint8_t CardCS = SS;
SdFatCard sd;
SdFatStorage sdStorage(sd);
PathVfs files(sdStorage);
PicoWiFiTransport transport;
esp32_nfs::Server nfs(files);
void setup() {
  Serial.begin(115200);
  if (!sd.begin(SdSpiConfig(CardCS, SHARED_SPI, SD_SCK_MHZ(12), &SPI)) || !sdStorage.refreshSpace())
    return;
  WiFi.begin(WifiSsid, WifiPassword);
  while (WiFi.status() != WL_CONNECTED)
    delay(50);
  Config config;
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  uint64_t epoch = (uint64_t(get_rand_32()) << 32) | get_rand_32();
  if (nfs.begin(transport, config, epoch ? epoch : 1))
    Serial.printf("NFS: %s:/\n", WiFi.localIP().toString().c_str());
}
void loop() {
  nfs.poll(millis());
  delay(1);
}
