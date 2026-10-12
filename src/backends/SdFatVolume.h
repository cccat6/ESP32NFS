// SPDX-License-Identifier: MIT
#pragma once
// Discover the Arduino dependency before its internal headers.
#include <SdFat.h>
#if SD_FAT_VERSION != 20300
#error "ESP32NFS SdFat adapter is validated against SdFat 2.3.0; pin that version."
#endif

#include "fat/Runtime.h"
namespace esp32_nfs {
// UTF-8 names and generic block I/O are fixed inside the filesystem implementation.
using SdFatVolume = fat::FsVolume;

// Explicit card initialization; the caller owns bus arbitration and mount lifetime.
class SdFatCard : public SdFatVolume {
public:
  SdFatCard() = default;
  SdFatCard(const SdFatCard &) = delete;
  SdFatCard &operator=(const SdFatCard &) = delete;
  ~SdFatCard() {
    SdFatVolume::end();
  }
  template <class Config> bool cardBegin(Config config) {
    bridge_.card = factory_.newCard(config);
    return bridge_.card && !bridge_.card->errorCode();
  }
  template <class Config> bool begin(Config config) {
    return cardBegin(config) && volumeBegin();
  }
  bool volumeBegin() {
    return bridge_.card && (SdFatVolume::begin(&bridge_) || SdFatVolume::begin(&bridge_, true, 0));
  }
  SdCard *card() {
    return bridge_.card;
  }
  FsBlockDeviceInterface *blockDevice() {
    return &bridge_;
  }
  void end() {
    SdFatVolume::end();
    if (bridge_.card)
      bridge_.card->end();
    bridge_.card = nullptr;
  }

private:
  class Bridge : public FsBlockDeviceInterface {
  public:
    SdCard *card = nullptr;
    bool isBusy() override {
      return card && card->isBusy();
    }
    uint32_t sectorCount() override {
      return card ? card->sectorCount() : 0;
    }
    bool syncDevice() override {
      return card && card->syncDevice();
    }
    bool readSector(uint32_t s, uint8_t *p) override {
      return card && card->readSector(s, p);
    }
    bool readSectors(uint32_t s, uint8_t *p, size_t n) override {
      return card && card->readSectors(s, p, n);
    }
    bool writeSector(uint32_t s, const uint8_t *p) override {
      return card && card->writeSector(s, p);
    }
    bool writeSectors(uint32_t s, const uint8_t *p, size_t n) override {
      return card && card->writeSectors(s, p, n);
    }
  } bridge_;
  SdCardFactory factory_;
};
} // namespace esp32_nfs
