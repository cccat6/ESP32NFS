#pragma once
// SPDX-License-Identifier: MIT
#include "storage/BlockDevice.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
namespace nfs_test {
class FileDevice : public esp32_nfs::storage::BlockDevice {
  int fd_;
  uint64_t blocks_;

public:
  unsigned sectorWrites = 0;
  explicit FileDevice(uint64_t blocks) : blocks_(blocks) {
    char name[] = "/tmp/esp32nfs-unicode-XXXXXX";
    fd_ = mkstemp(name);
    assert(fd_ >= 0);
    unlink(name);
    assert(ftruncate(fd_, blocks * 512) == 0);
  }
  ~FileDevice() {
    close(fd_);
  }
  esp32_nfs::storage::Geometry geometry() const override {
    esp32_nfs::storage::Geometry g;
    g.blocks = blocks_;
    g.identity = 1234;
    g.generation = 1;
    g.writable = g.durableFlush = g.isolatedWrites = true;
    return g;
  }
  esp32_nfs::storage::Result start(const esp32_nfs::storage::Request &r) override {
    using namespace esp32_nfs::storage;
    if (r.operation == Operation::Flush)
      return {fsync(fd_) ? Error::Io : Error::Ok, 0, true};
    if (r.lba > blocks_ || r.blocks > blocks_ - r.lba || r.bufferBytes < size_t(r.blocks) * 512)
      return {Error::Invalid};
    if (r.operation == Operation::Write)
      ++sectorWrites;
    ssize_t n = r.operation == Operation::Read
                    ? pread(fd_, r.readBuffer, r.blocks * 512, uint64_t(r.lba) * 512)
                    : pwrite(fd_, r.writeBuffer, r.blocks * 512, uint64_t(r.lba) * 512);
    return {n == ssize_t(r.blocks * 512) ? Error::Ok : Error::Io, n < 0 ? 0 : uint32_t(n / 512)};
  }
  esp32_nfs::storage::Result poll() override {
    return {esp32_nfs::storage::Error::Invalid};
  }
  void cancel() override {}
};
} // namespace nfs_test
#include "backends/SdFatStorage.h"
#include "storage/SdFatBlockAdapter.h"
#include <cstdio>
#include <set>
#include <string>
#include <vector>
using namespace esp32_nfs;
using namespace esp32_nfs::storage;
struct Fixture {
  static constexpr uint64_t Sectors = 16 * 1024 * 1024;
  nfs_test::FileDevice raw{Sectors + SectorJournal::RequiredJournalBlocks};
  BlockIo base{raw};
  SectorJournal journal{base, 0, Sectors, Sectors};
  BlockIo tx{journal};
  SdFatBlockAdapter disk{tx, &journal};
  FsVolume volume;
  SdFatStorage backend{volume};
  PathVfs fs{backend};
  static bool remount(void *p) {
    auto &f = *static_cast<Fixture *>(p);
    f.volume.end();
    return f.volume.begin(&f.disk, false, 1);
  }
  Fixture(bool exfat, bool transactional) {
    assert(journal.formatJournal() == Error::Ok);
    uint8_t scratch[512];
    if (exfat) {
      ExFatFormatter f;
      assert(f.format(&disk, scratch, nullptr));
    } else {
      FatFormatter f;
      assert(f.format(&disk, scratch, nullptr));
    }
    assert(volume.begin(&disk, false, 1));
    if (transactional)
      backend.attachTransactions(journal, remount, this);
  }
  std::string alias(const char *path) {
    FsFile file = volume.open(path, O_RDONLY);
    assert(file);
    std::string parent = path;
    parent.resize(parent.rfind('/'));
    if (parent.empty())
      parent = "/";
    FsFile dir = volume.open(parent.c_str(), O_RDONLY);
    assert(dir);
    assert(dir.seekSet(uint64_t(file.dirIndex()) * 32));
    uint8_t record[32];
    assert(dir.read(record, 32) == 32);
    std::string base(reinterpret_cast<char *>(record), 8),
        ext(reinterpret_cast<char *>(record + 8), 3);
    while (!base.empty() && base.back() == ' ')
      base.pop_back();
    while (!ext.empty() && ext.back() == ' ')
      ext.pop_back();
    file.close();
    dir.close();
    return base + (ext.empty() ? "" : "." + ext);
  }
};
