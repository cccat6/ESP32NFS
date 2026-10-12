// SPDX-License-Identifier: MIT
#pragma once
#include "ExportMap.h"
#include "Memory.h"
#include <stddef.h>
#include <stdint.h>

#ifndef ESP32_NFS_REPLAY_COUNT
#define ESP32_NFS_REPLAY_COUNT 4
#endif
#ifndef ESP32_NFS_CONNECTION_COUNT
#define ESP32_NFS_CONNECTION_COUNT 2
#endif

namespace esp32_nfs {
static constexpr size_t BlockSize = 4096;
static constexpr size_t MaxBlockSize = 32768;
static constexpr size_t RecordSize = BlockSize + 512;
static constexpr size_t PathSize = 256;
static constexpr size_t ReplayReplySize = 384;
static_assert(ESP32_NFS_REPLAY_COUNT > 0, "Mutating RPCs require replay slots");
static_assert(ESP32_NFS_CONNECTION_COUNT > 0, "At least one connection required");

enum Status : uint32_t {
  Ok = 0,
  Perm = 1,
  NoEnt = 2,
  Io = 5,
  Access = 13,
  Exist = 17,
  Xdev = 18,
  NotDir = 20,
  IsDir = 21,
  Invalid = 22,
  FBig = 27,
  NoSpace = 28,
  ReadOnly = 30,
  NameTooLong = 63,
  NotEmpty = 66,
  Stale = 70,
  BadHandle = 10001,
  BadCookie = 10003,
  NotSupported = 10004,
  TooSmall = 10005,
  ServerFault = 10006,
  NotSync = 10002,
  Jukebox = 10008
};
struct Time {
  uint32_t seconds = 0, nanos = 0;
};
struct Attr {
  uint32_t type = 1, mode = 0666, links = 1, uid = 0, gid = 0;
  uint64_t size = 0, used = 0, fsid = 1, id = 0;
  Time atime, mtime, ctime;
};
struct SetAttr {
  bool modeSet = false, uidSet = false, gidSet = false, sizeSet = false;
  uint32_t mode = 0, uid = 0, gid = 0, atimeHow = 0, mtimeHow = 0;
  uint64_t size = 0;
  Time atime, mtime;
};
struct Space {
  uint64_t totalBytes = 0, freeBytes = 0, totalFiles = 0, freeFiles = 0;
};
struct Entry {
  uint64_t id = 0, cookie = 0;
  char name[PathSize] = {};
  Attr attr;
};

// Caller owns the mounted volume. No SPI initialization occurs in this library.
class Lease {
public:
  virtual ~Lease() {}
  virtual bool acquire() = 0; // Must return promptly; false returns RPC SYSTEM_ERR for retry.
  virtual void release() = 0;
};
class Vfs {
public:
  virtual ~Vfs() {}
  virtual uint64_t root() const = 0;
  virtual uint64_t generation() const {
    return 1;
  }
  virtual Status getattr(uint64_t id, Attr &) = 0;
  virtual Status lookup(uint64_t dir, const char *name, uint64_t &id) = 0;
  virtual Status setattr(uint64_t id, const SetAttr &) = 0;
  virtual Status read(uint64_t id, uint64_t offset, uint8_t *, size_t &count) = 0;
  virtual Status write(uint64_t id, uint64_t offset, const uint8_t *, size_t &count) = 0;
  // Optional write-back: only UNSTABLE replies may acknowledge this path before sync.
  virtual bool bufferedWrites() const {
    return false;
  }
  virtual Status writeBuffered(uint64_t id, uint64_t off, const uint8_t *p, size_t &n) {
    return write(id, off, p, n);
  }
  // exclusive=1 is GUARDED; exclusive=2 is EXCLUSIVE with persistent verifier semantics.
  virtual Status create(uint64_t dir, const char *, bool directory, uint32_t exclusive,
                        uint64_t verifier, const SetAttr &, uint64_t &id) = 0;
  virtual Status remove(uint64_t dir, const char *, bool directory) = 0;
  virtual Status rename(uint64_t from, const char *, uint64_t to, const char *) = 0;
  virtual Status readdir(uint64_t dir, uint64_t cookie, uint64_t verifier, Entry &,
                         bool &eof) = 0; // One entry, cookie is opaque; no stack-sized pages.
  virtual Status readdirNames(uint64_t d, uint64_t cookie, uint64_t verifier, Entry &e, bool &eof) {
    return readdir(d, cookie, verifier, e, eof);
  }
  virtual Status directoryVerifier(uint64_t dir, uint64_t &verifier) = 0;
  virtual Status sync(uint64_t id) = 0; // durable file data and metadata, or IO error
  virtual Status space(Space &) = 0;
  virtual uint64_t maxFileSize() const = 0;
  virtual bool stableStorage() const {
    return true;
  }
  virtual bool writable() const {
    return true;
  }
  virtual bool caseInsensitive() const {
    return false;
  }
  virtual bool canSetTimes() const {
    return false;
  }
  virtual Status endSession() {
    return Ok;
  }
};
struct Config {
  const char *exportPath = "/"; // Copied by begin().
  bool readOnly = false;
  bool portmapper = false; // Optional PMAP v2 TCP dispatch on the same listener.
  uint16_t port = 2049;
  uint32_t blockSize = BlockSize;
  uint32_t bytesPerSecond = 0; // 0 disables the token bucket.
  uint32_t ioBytesPerPoll = 8192;
  uint32_t directoryEntriesPerRpc = 16;
  uint32_t idleTimeoutMs = 30000, recordTimeoutMs = 5000, replayWindowMs = 60000;
  uint8_t connections = ESP32_NFS_CONNECTION_COUNT;
};
class Transport {
public:
  virtual ~Transport() {}
  virtual bool listen(uint16_t port) = 0;
  // Socket IDs are transport-owned. peer is a stable address identity (no source port).
  virtual int accept(uint64_t &peer) = 0;
  virtual bool connected(int socket) = 0;
  virtual int available(int socket) = 0;
  virtual int read(int socket, uint8_t *, size_t) = 0;
  virtual int write(int socket, const uint8_t *, size_t) = 0; // nonblocking, bounded
  virtual void close(int socket) = 0;
  virtual void stop() = 0;
};
struct Metrics {
  uint64_t received = 0, sent = 0;
  uint32_t requests = 0, replayHits = 0, rejected = 0, timeouts = 0, memoryFailures = 0;
  size_t ownedHeap = 0, peakOwnedHeap = 0;
};
class Server {
public:
  explicit Server(Vfs &, Lease *lease = nullptr, Memory &memory = defaultMemory());
  explicit Server(ExportMap &, Lease *defaultLease = nullptr, Memory &memory = defaultMemory());
  ExportMap &exports() {
    return *exports_;
  }
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  // epoch MUST change after restart, remount, or lost writes. Nonzero random boot nonce
  // recommended.
  bool begin(Transport &, const Config &, uint64_t epoch);
  Status stop();
  Status lastStatus() const {
    return lastStatus_;
  }
  bool unmount(const char *path) {
    return exports_->remove(path, lease_);
  }
  // Cooperative, bounded network work. One storage RPC per poll; SD latency is backend-dependent.
  void poll(uint32_t nowMs);
  uint32_t suggestedWaitMs() const;
  const Metrics &metrics() const {
    return metrics_;
  }
  static size_t activeWorkspaceBytes(size_t payload = BlockSize);
  // Host/fuzz harness: execute an unframed RPC. Same bounded replay cache as TCP.
  size_t rpc(const uint8_t *, size_t, uint8_t *, size_t, uint64_t peer, uint32_t nowMs);

private:
  struct Workspace;
  struct Connection {
    int socket = -1;
    uint64_t peer = 0;
    uint32_t last = 0, started = 0, remaining = 0, fragments = 0;
    uint8_t marker[4] = {};
    uint8_t markerUsed = 0;
    bool finalFragment = false;
    size_t received = 0, sending = 0, replyLength = 0;
  };
  Memory &memory_;
  Status lastStatus_ = Ok;
  bool stopPending_ = false;
  Vfs *defaultVfs_;
  ExportMap ownExports_;
  ExportMap *exports_;
  uint64_t mappingRevision_ = 0;
  Lease *lease_;
  Transport *transport_ = nullptr;
  Workspace *work_ = nullptr;
  Config config_;
  Metrics metrics_;
  Connection connections_[ESP32_NFS_CONNECTION_COUNT];
  uint64_t epoch_ = 0, peer_ = 0;
  bool peerSet_ = false;
  uint64_t dispatchPeer_ = 0;
  int owner_ = -1;
  unsigned nextConnection_ = 0;
  uint32_t tokenTime_ = 0, tokens_ = 0, tokenFraction_ = 0;
  bool ensureWorkspace();
  void releaseWorkspace(bool buffersOnly = false);
  void drop(unsigned);
  size_t dispatch(const uint8_t *, size_t, uint8_t *, size_t);
};
} // namespace esp32_nfs
