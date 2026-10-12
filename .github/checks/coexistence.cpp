// Compile both filesystem implementations with the dependency's default name configuration.
#include <SdFat.h>
#include <backends/SdFatStorage.h>
static_assert(USE_UTF8_LONG_NAMES == 0, "No application UTF-8 build flags");
SdFs ordinary;
esp32_nfs::SdFatCard exported;
esp32_nfs::SdFatStorage storage(exported);
esp32_nfs::PathVfs files(storage);
void setup() {
  SdSpiConfig config(SS, SHARED_SPI, SD_SCK_MHZ(12), &SPI);
  if (ordinary.begin(config)) {
    auto file = ordinary.open("/stock.txt", O_RDONLY);
    if (file)
      file.close();
  }
  ordinary.end();
  if (exported.begin(config)) {
    auto file = exported.open("/中文.txt", O_RDONLY);
    if (file)
      file.close();
    storage.refreshSpace();
  }
  exported.end();
}
void loop() {}
