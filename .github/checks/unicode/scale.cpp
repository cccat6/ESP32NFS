// SPDX-License-Identifier: MIT
#include "Xdr.h"
#include "backends/LittleFsStorage.h"
#include "fixture.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <thread>
// Metadata-only backend keeps the identity/lifecycle checks independent of physical I/O.
class Files : public PathStorage {
public:
  std::map<std::string, bool> files{{"/", true}};
  unsigned creates = 0, releases = 0;
  Status releaseStatus = Ok;
  Status stat(const char *p, Attr &a) override {
    auto i = files.find(p);
    if (i == files.end())
      return NoEnt;
    a = Attr();
    a.type = i->second ? 2 : 1;
    return Ok;
  }
  Status setattr(const char *, const SetAttr &) override {
    return Ok;
  }
  Status read(const char *, uint64_t, uint8_t *, size_t &n) override {
    n = 0;
    return Ok;
  }
  Status write(const char *, uint64_t, const uint8_t *, size_t &) override {
    return Ok;
  }
  Status create(const char *p, bool dir) override {
    ++creates;
    return files.emplace(p, dir).second ? Ok : Exist;
  }
  Status remove(const char *p, bool) override {
    return files.erase(p) ? Ok : NoEnt;
  }
  Status rename(const char *a, const char *b) override {
    std::vector<std::pair<std::string, bool>> moved;
    for (auto &e : files)
      if (e.first == a || name_detail::below(e.first.c_str(), a))
        moved.emplace_back(std::string(b) + e.first.substr(strlen(a)), e.second);
    if (moved.empty())
      return NoEnt;
    for (auto i = files.begin(); i != files.end();)
      if (i->first == a || name_detail::below(i->first.c_str(), a))
        i = files.erase(i);
      else
        ++i;
    for (auto &e : moved)
      files[e.first] = e.second;
    return Ok;
  }
  Status entry(const char *p, uint64_t index, char *name, size_t cap, bool &eof) override {
    std::string prefix = std::string(p) + (strcmp(p, "/") ? "/" : "");
    for (auto &e : files)
      if (e.first.compare(0, prefix.size(), prefix) == 0) {
        auto leaf = e.first.substr(prefix.size());
        if (leaf.empty() || leaf.find('/') != std::string::npos)
          continue;
        if (index--)
          continue;
        if (leaf.size() >= cap)
          return NameTooLong;
        strcpy(name, leaf.c_str());
        eof = false;
        return Ok;
      }
    eof = true;
    return Ok;
  }
  Status sync(const char *) override {
    return Ok;
  }
  Status space(Space &s) override {
    s = Space();
    return Ok;
  }
  uint64_t maxFileSize() const override {
    return UINT32_MAX;
  }
  Status releaseResources() override {
    ++releases;
    return releaseStatus;
  }
};
class Network : public Transport {
public:
  bool active = false;
  bool listen(uint16_t) override {
    return active = true;
  }
  int accept(uint64_t &) override {
    return -1;
  }
  bool connected(int) override {
    return false;
  }
  int available(int) override {
    return 0;
  }
  int read(int, uint8_t *, size_t) override {
    return 0;
  }
  int write(int, const uint8_t *, size_t) override {
    return 0;
  }
  void close(int) override {}
  void stop() override {
    active = false;
  }
};
class Gate : public Lease {
public:
  bool ready = true;
  unsigned held = 0;
  bool acquire() override {
    if (!ready)
      return false;
    ++held;
    return true;
  }
  void release() override {
    assert(held);
    --held;
  }
};
struct FaultHeap {
  int remaining = -1;
  size_t available = SIZE_MAX;
  size_t live = 0;
  static void *allocate(void *p, size_t n) {
    auto &h = *static_cast<FaultHeap *>(p);
    if (h.remaining == 0)
      return nullptr;
    if (h.remaining > 0)
      --h.remaining;
    void *v = malloc(n);
    if (v)
      ++h.live;
    return v;
  }
  static void release(void *p, void *v) {
    --static_cast<FaultHeap *>(p)->live;
    free(v);
  }
  static size_t freeBytes(void *p) {
    return static_cast<FaultHeap *>(p)->available;
  }
  Memory::Allocator allocator() {
    Memory::Allocator a;
    a.context = this;
    a.allocate = allocate;
    a.release = release;
    a.available = freeBytes;
    return a;
  }
};
static void lifecycle() {
  MemoryLimits limits;
  FaultHeap heap;
  Memory memory(limits, heap.allocator());
  Files storage;
  PathVfs::Registry registry(memory);
  PathVfs fs(storage, registry);
  Gate gate;
  Network network;
  Server server(fs, &gate, memory);
  Config config;
  assert(server.begin(network, config, 1));
  uint64_t first = 0;
  assert(fs.create(fs.root(), "old", false, 0, 0, SetAttr(), first) == Ok);
  uint8_t request[40], reply[RecordSize];
  XdrOut x(request, sizeof(request));
  for (uint32_t v : {1u, 0u, 2u, 100003u, 3u, 0u, 0u, 0u, 0u, 0u})
    x.u32(v);
  size_t base = memory.stats().usedBytes;
  for (int n = 0; n < 3; ++n) {
    heap.remaining = n;
    assert(server.rpc(request, x.pos, reply, sizeof(reply), 1, 1) == 24);
    assert(server.lastStatus() == Jukebox);
    assert(memory.stats().usedBytes == base);
  }
  heap.remaining = -1;
  assert(server.rpc(request, x.pos, reply, sizeof(reply), 1, 1) == 24);
  assert(server.lastStatus() == Ok);
  Config invalid = config;
  invalid.connections = 0;
  assert(!server.begin(network, invalid, 2) && network.active);
  invalid = config;
  invalid.exportPath = "/invalid/";
  assert(!server.begin(network, invalid, 2) && network.active);
  gate.ready = false;
  unsigned released = storage.releases;
  assert(server.stop() == Jukebox && storage.releases == released);
  assert(registry.allocatedBytes() > 0 && gate.held == 0);
  gate.ready = true;
  assert(server.stop() == Ok && memory.stats().usedBytes == 0 && heap.live == 0);
  Attr a;
  assert(fs.getattr(first, a) == Stale);
  assert(server.begin(network, config, 2));
  uint64_t next;
  assert(fs.lookup(fs.root(), "old", next) == Ok && next != first);
  storage.releaseStatus = Io;
  assert(server.stop() == Io && memory.stats().usedBytes == 0);
  storage.releaseStatus = Ok;
  assert(server.stop() == Ok);
  config.blockSize = 16384;
  assert(server.begin(network, config, 3));
  assert(server.rpc(request, x.pos, reply, sizeof(reply), 1, 2) == 24);
  assert(server.metrics().ownedHeap == Server::activeWorkspaceBytes(16384));
  assert(server.stop() == Ok && memory.stats().usedBytes == 0);
}
static void failures() {
  FaultHeap heap;
  MemoryLimits limits;
  Memory memory(limits, heap.allocator());
  Files storage;
  PathVfs::Registry registry(memory);
  PathVfs fs(storage, registry);
  uint64_t id;
  Attr a;
  heap.remaining = 0;
  assert(fs.getattr(fs.root(), a) == Jukebox && memory.stats().usedBytes == 0);
  heap.remaining = 1;
  assert(fs.getattr(fs.root(), a) == Jukebox && memory.stats().usedBytes == 0);
  heap.remaining = -1;
  heap.available = 32 * 1024;
  assert(fs.getattr(fs.root(), a) == Jukebox && memory.stats().lastError == MemoryError::Reserve);
  heap.available = SIZE_MAX;
  assert(fs.create(fs.root(), "source", false, 0, 0, SetAttr(), id) == Ok);
  uint64_t held = id;
  heap.remaining = 0;
  auto creates = storage.creates;
  assert(fs.create(fs.root(), "denied", false, 0, 0, SetAttr(), id) == Jukebox);
  assert(storage.creates == creates && !storage.files.count("/denied"));
  assert(fs.rename(fs.root(), "source", fs.root(), "a-much-longer-name") == Jukebox);
  assert(storage.files.count("/source") && fs.getattr(held, a) == Ok);
  heap.remaining = -1;
  size_t used = memory.stats().usedBytes;
  MemoryLimits tooSmall;
  tooSmall.maxBytes = used - 1;
  MemoryLimits unchanged;
  assert(!memory.configure(tooSmall) && memory.getLimits(unchanged) &&
         unchanged.maxBytes == limits.maxBytes);
  MemoryLimits full;
  full.maxBytes = used;
  assert(memory.configure(full));
  assert(fs.create(fs.root(), "budget", false, 0, 0, SetAttr(), id) == Jukebox);
  assert(memory.stats().lastError == MemoryError::Budget);
  assert(memory.configure(limits));
  assert(fs.endSession() == Ok && memory.stats().usedBytes == 0);
  assert(fs.getattr(held, a) == Stale);
}
static void large(bool exfat) {
  constexpr unsigned Count = 1100;
  Fixture f(exfat, false);
  assert(f.volume.mkdir("/目录", false));
  for (unsigned i = 0; i < Count; ++i) {
    char p[96];
    snprintf(p, sizeof(p), "/目录/F%04u-中文.txt", i);
    auto file = f.volume.open(p, O_CREAT | O_EXCL | O_RDWR);
    bool opened = bool(file);
    size_t written = opened ? file.write("x", 1) : 0;
    bool closed = file.close();
    if (!opened || written != 1 || !closed)
      fprintf(stderr, "seed %s %u: open=%d write=%zu close=%d\n", exfat ? "exfat" : "fat", i,
              opened, written, closed);
    assert(opened && written == 1 && closed);
  }
  auto &fs = f.fs;
  uint64_t directory;
  assert(fs.lookup(fs.root(), "目录", directory) == Ok);
  uint64_t verifier;
  assert(fs.directoryVerifier(directory, verifier) == Ok);
  std::vector<uint64_t> ids;
  unsigned before = f.raw.sectorWrites;
  auto began = std::chrono::steady_clock::now();
  for (uint64_t cookie = 0;;) {
    Entry e;
    bool eof = false;
    Status result;
    unsigned retries = 0;
    do {
      result = fs.readdir(directory, cookie, verifier, e, eof);
    } while (result == Jukebox && ++retries < Count);
    if (result != Ok)
      fprintf(stderr, "enumerate %llu: status=%u bytes=%zu memory_error=%u\n",
              (unsigned long long)cookie, unsigned(result), fs.registryBytes(),
              unsigned(defaultMemory().stats().lastError));
    assert(result == Ok);
    if (eof)
      break;
    ids.push_back(e.id);
    cookie = e.cookie;
    // Force a cursor reopen at page boundaries; no identity or cookie is discarded.
    if (cookie % 127 == 0)
      assert(fs.releaseCaches() == Ok);
  }
  assert(ids.size() == Count && f.raw.sectorWrites == before);
  uint64_t first;
  assert(fs.lookup(directory, "F0000-中文.txt", first) == Ok && first == ids[0]);
  if (!exfat) {
    std::string alias = f.alias("/目录/F0000-中文.txt");
    uint64_t other;
    assert(fs.lookup(directory, alias.c_str(), other) == Ok && other == first);
  }
  assert(fs.rename(fs.root(), "目录", fs.root(), "改名目录") == Ok);
  char p[PathSize];
  assert(fs.canonicalPath(first, p, sizeof(p)) == Ok);
  assert(!strcmp(p, "/改名目录/F0000-中文.txt"));
  uint64_t again;
  assert(fs.lookup(directory, "F0000-中文.txt", again) == Ok && again == first);
  size_t peak = fs.registryBytes();
  // The end of the paged scan retains usable handles to every earlier entry.
  for (unsigned i = 0; i < Count; ++i) {
    Attr a;
    assert(fs.getattr(ids[i], a) == Ok);
  }
  for (unsigned i = 0; i < Count; ++i) {
    char name[64];
    snprintf(name, sizeof(name), "F%04u-中文.txt", i);
    assert(fs.remove(directory, name, false) == Ok);
  }
  assert(fs.registryBytes() < peak / 4);
  assert(fs.endSession() == Ok && fs.registryBytes() == 0);
  Attr a;
  assert(fs.getattr(first, a) == Stale);
  double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
  printf("%s: %u entries, identity peak=%zu bytes, scan/rename/lookup/delete=%.3fs\n",
         exfat ? "exFAT" : "FAT32", Count, peak, seconds);
}
static void exclusive(bool exfat) {
  Fixture f(exfat, true);
  std::vector<uint64_t> ids;
  for (unsigned i = 0; i < 130; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "E%04u.txt", i);
    uint64_t id;
    assert(f.fs.create(f.fs.root(), name, false, 2, 100 + i, SetAttr(), id) == Ok);
    ids.push_back(id);
  }
  assert(f.fs.releaseCaches() == Ok);
  for (unsigned i = 0; i < 130; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "E%04u.txt", i);
    uint64_t id;
    assert(f.fs.create(f.fs.root(), name, false, 2, 100 + i, SetAttr(), id) == Ok && id == ids[i]);
    assert(f.fs.create(f.fs.root(), name, false, 2, 9999, SetAttr(), id) == Exist);
  }
  assert(f.fs.endSession() == Ok && defaultMemory().stats().usedBytes == 0);
}
static void concurrentMemory() {
  MemoryLimits limits;
  limits.maxBytes = 2048;
  limits.reserveBytes = 0;
  Memory memory(limits);
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 4; ++i)
    threads.emplace_back([&]() {
      for (unsigned j = 0; j < 1000; ++j) {
        void *p;
        while (!(p = memory.allocate(128)))
          std::this_thread::yield();
        memset(p, 0xa5, 128);
        assert(memory.stats().usedBytes <= limits.maxBytes);
        memory.release(p, 128);
      }
    });
  for (auto &t : threads)
    t.join();
  assert(memory.stats().usedBytes == 0 && memory.stats().allocations == 0);
}
static void invalidInputs() {
  uint8_t data[512] = {}, source[512];
  memset(source, 0xa5, sizeof(source));
  MemoryDevice device(data, sizeof(data));
  Request request;
  request.operation = static_cast<Operation>(99);
  request.generation = device.geometry().generation;
  request.blocks = 1;
  request.writeBuffer = source;
  request.bufferBytes = sizeof(source);
  assert(device.start(request).error == Error::Invalid && data[0] == 0);
  MemoryDevice absent(nullptr, 512);
  request.operation = Operation::Read;
  request.readBuffer = data;
  assert(absent.start(request).error == Error::NoMedia);
  XdrOut output(data, sizeof(data));
  output.pos = sizeof(data) + 1;
  output.u32(1);
  assert(!output.valid);
  XdrIn input(data, sizeof(data));
  input.pos = sizeof(data) + 1;
  input.u32();
  assert(!input.valid);
  XdrIn empty(data, sizeof(data));
  char name[1] = {0x55};
  assert(!empty.string(name, 0) && name[0] == 0x55);
  Files storage;
  PathVfs fs(storage);
  uint64_t id;
  assert(fs.create(fs.root(), nullptr, false, 0, 0, SetAttr(), id) == Invalid);
  assert(fs.rename(fs.root(), nullptr, fs.root(), "x") == Invalid);
  assert(fs.remove(fs.root(), nullptr, false) == Invalid);
  assert(storage.creates == 0);
}
int main() {
  invalidInputs();
  concurrentMemory();
  lifecycle();
  failures();
  large(false);
  large(true);
  exclusive(false);
  exclusive(true);
  assert(defaultMemory().stats().usedBytes == 0);
  puts("resource, lifecycle, large-directory checks passed");
}
