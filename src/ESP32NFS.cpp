// SPDX-License-Identifier: MIT
#include "ESP32NFS.h"
#include "Digest.h"
#include "Xdr.h"
#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>
namespace esp32_nfs {
struct Server::Workspace {
  uint8_t *request = nullptr;
  uint8_t *reply = nullptr;
  ~Workspace() {
    delete[] request;
    delete[] reply;
  }
  struct Replay {
    bool used = false;
    uint64_t peer = 0;
    uint32_t time = 0, xid = 0;
    size_t requestSize = 0, replySize = 0;
    uint8_t digest[32], reply[ReplayReplySize];
  };
  Replay replay[ESP32_NFS_REPLAY_COUNT];
};
namespace {
bool setattrs(XdrIn &x, SetAttr &a) {
  a.modeSet = x.boolean();
  if (a.modeSet)
    a.mode = x.u32();
  a.uidSet = x.boolean();
  if (a.uidSet)
    a.uid = x.u32();
  a.gidSet = x.boolean();
  if (a.gidSet)
    a.gid = x.u32();
  a.sizeSet = x.boolean();
  if (a.sizeSet)
    a.size = x.u64();
  a.atimeHow = x.u32();
  if (a.atimeHow > 2)
    x.valid = false;
  if (a.atimeHow == 2) {
    a.atime.seconds = x.u32();
    a.atime.nanos = x.u32();
    if (a.atime.nanos >= 1000000000)
      x.valid = false;
  }
  a.mtimeHow = x.u32();
  if (a.mtimeHow > 2)
    x.valid = false;
  if (a.mtimeHow == 2) {
    a.mtime.seconds = x.u32();
    a.mtime.nanos = x.u32();
    if (a.mtime.nanos >= 1000000000)
      x.valid = false;
  }
  return x.valid;
}
void time(XdrOut &x, const Time &t) {
  x.u32(t.seconds);
  x.u32(t.nanos);
}
void attr(XdrOut &x, const Attr &a, uint64_t volume) {
  x.u32(a.type);
  x.u32(a.mode);
  x.u32(a.links);
  x.u32(a.uid);
  x.u32(a.gid);
  x.u64(a.size);
  x.u64(a.used);
  x.u32(0);
  x.u32(0);
  x.u64(volume);
  x.u64(a.id);
  time(x, a.atime);
  time(x, a.mtime);
  time(x, a.ctime);
}
void post(XdrOut &x, Vfs *v, uint64_t id, uint64_t volume) {
  Attr a;
  bool ok = v && id && v->getattr(id, a) == Ok;
  x.u32(ok);
  if (ok)
    attr(x, a, volume);
}
void wcc(XdrOut &x, Vfs *v, uint64_t id, const Attr *before, uint64_t volume) {
  x.u32(before != nullptr);
  if (before) {
    x.u64(before->size);
    time(x, before->mtime);
    time(x, before->ctime);
  }
  post(x, v, id, volume);
}
void handle(XdrOut &x, uint64_t epoch, uint64_t volume, uint64_t id) {
  x.u32(24);
  x.u64(epoch);
  x.u64(volume);
  x.u64(id);
}
Status gethandle(XdrIn &x, uint64_t epoch, ExportMap &exports, ExportMap::Mapping *&mapping,
                 uint64_t &id) {
  size_t n;
  const uint8_t *p = x.opaque(n, 64);
  if (!p)
    return BadHandle;
  if (n != 24)
    return BadHandle;
  XdrIn h(p, n);
  if (h.u64() != epoch)
    return Stale;
  mapping = exports.find(h.u64());
  id = h.u64();
  return !mapping ? Stale : id ? Ok : BadHandle;
}
bool auth(XdrIn &x, bool credential) {
  uint32_t flavor = x.u32();
  size_t n;
  const uint8_t *p = x.opaque(n, 400);
  if (!p)
    return false;
  if (flavor == 0)
    return n == 0;
  if (!credential || flavor != 1)
    return false;
  XdrIn a(p, n);
  a.u32();
  char machine[256];
  a.string(machine, sizeof(machine));
  a.u32();
  a.u32();
  uint32_t groups = a.u32();
  if (groups > 16)
    return false;
  for (uint32_t i = 0; i < groups; ++i)
    a.u32();
  return a.done();
}
} // namespace
Server::Server(Vfs &v, Lease *l) : defaultVfs_(&v), exports_(&ownExports_), lease_(l) {}
Server::Server(ExportMap &e, Lease *l) : defaultVfs_(nullptr), exports_(&e), lease_(l) {}
Server::~Server() {
  stop();
}
size_t Server::activeWorkspaceBytes() {
  return sizeof(Workspace) + 2 * RecordSize + 4;
}
bool Server::ensureWorkspace() {
  if (!work_)
    work_ = new (std::nothrow) Workspace;
  if (!work_)
    return false;
  if (!work_->request)
    work_->request = new (std::nothrow) uint8_t[RecordSize];
  if (!work_->reply)
    work_->reply = new (std::nothrow) uint8_t[RecordSize + 4];
  metrics_.ownedHeap =
      sizeof(Workspace) + (work_->request ? RecordSize : 0) + (work_->reply ? RecordSize + 4 : 0);
  metrics_.peakOwnedHeap = std::max(metrics_.peakOwnedHeap, metrics_.ownedHeap);
  return work_->request && work_->reply;
}
bool Server::begin(Transport &t, const Config &c, uint64_t epoch) {
  stop();
  if (!epoch || !c.exportPath || strlen(c.exportPath) >= ExportMap::PathCapacity ||
      c.exportPath[0] != '/' || c.blockSize > BlockSize || c.blockSize < 512 ||
      !c.directoryEntriesPerRpc || !c.connections || c.connections > ESP32_NFS_CONNECTION_COUNT ||
      !c.ioBytesPerPoll || !c.recordTimeoutMs || c.recordTimeoutMs > INT32_MAX ||
      !c.idleTimeoutMs || c.idleTimeoutMs > INT32_MAX || !c.replayWindowMs ||
      c.replayWindowMs > INT32_MAX)
    return false;
  config_ = c;
  if (defaultVfs_ && !ownExports_.add(c.exportPath, *defaultVfs_))
    return false;
  mappingRevision_ = exports_->revision();
  epoch_ = epoch;
  peerSet_ = false;
  metrics_ = Metrics();
  if (!t.listen(c.port))
    return false;
  transport_ = &t;
  return true;
}
void Server::stop() {
  if (transport_) {
    for (unsigned i = 0; i < ESP32_NFS_CONNECTION_COUNT; ++i)
      if (connections_[i].socket >= 0)
        transport_->close(connections_[i].socket);
    transport_->stop();
  }
  if (defaultVfs_)
    ownExports_.clear();
  else
    exports_->endSessions();
  transport_ = nullptr;
  for (auto &c : connections_)
    c = Connection();
  delete work_;
  work_ = nullptr;
  metrics_.ownedHeap = 0;
  owner_ = -1;
  peerSet_ = false;
  epoch_ = 0;
  tokens_ = 0;
  tokenFraction_ = 0;
  tokenTime_ = 0;
  nextConnection_ = 0;
}
void Server::drop(unsigned i) {
  if (transport_ && connections_[i].socket >= 0)
    transport_->close(connections_[i].socket);
  connections_[i] = Connection();
  if (owner_ == int(i))
    owner_ = -1;
}
uint32_t Server::suggestedWaitMs() const {
  return owner_ < 0 ? 10 : 1;
}
void Server::poll(uint32_t now) {
  if (!transport_)
    return;
  for (unsigned i = 0; i < config_.connections; ++i)
    if (connections_[i].socket >= 0) {
      auto &c = connections_[i];
      if (!transport_->connected(c.socket) || uint32_t(now - c.last) >= config_.idleTimeoutMs ||
          (owner_ == int(i) && uint32_t(now - c.started) >= config_.recordTimeoutMs)) {
        ++metrics_.timeouts;
        drop(i);
      }
    }
  bool active = false;
  for (auto &c : connections_)
    active |= c.socket >= 0;
  // Retain exact replay records across reconnects for the configured replay window.
  if (!active && work_) {
    delete[] work_->request;
    delete[] work_->reply;
    work_->request = work_->reply = nullptr;
    metrics_.ownedHeap = sizeof(Workspace);
    bool live = false;
    for (auto &r : work_->replay)
      live |= r.used && uint32_t(now - r.time) < config_.replayWindowMs;
    if (!live) {
      delete work_;
      work_ = nullptr;
      metrics_.ownedHeap = 0;
      peerSet_ = false;
    }
  }
  uint64_t peer;
  int incoming = transport_->accept(peer);
  if (incoming >= 0) {
    int slot = -1;
    for (unsigned i = 0; i < config_.connections; ++i)
      if (connections_[i].socket < 0) {
        slot = i;
        break;
      }
    if (slot < 0 || (peerSet_ && peer_ != peer)) {
      transport_->close(incoming);
      ++metrics_.rejected;
    } else {
      peer_ = peer;
      peerSet_ = true;
      connections_[slot].socket = incoming;
      connections_[slot].peer = peer;
      connections_[slot].last = now;
    }
  }
  if (owner_ < 0) {
    for (unsigned step = 0; step < config_.connections; ++step) {
      unsigned i = (nextConnection_ + step) % config_.connections;
      if (connections_[i].socket >= 0 && transport_->available(connections_[i].socket) > 0) {
        if (!ensureWorkspace()) {
          drop(i);
          return;
        }
        owner_ = i;
        nextConnection_ = (i + 1) % config_.connections;
        connections_[i].started = now;
        break;
      }
    }
  }
  if (owner_ < 0)
    return;
  auto &c = connections_[owner_];
  size_t budget = config_.ioBytesPerPoll;
  if (config_.bytesPerSecond) {
    uint32_t elapsed = now - tokenTime_;
    tokenTime_ = now;
    uint64_t scaled = uint64_t(elapsed) * config_.bytesPerSecond + tokenFraction_;
    uint64_t add = scaled / 1000;
    tokenFraction_ = scaled % 1000;
    tokens_ = uint32_t(std::min<uint64_t>(uint64_t(tokens_) + add, config_.bytesPerSecond));
    budget = std::min<size_t>(budget, tokens_);
  }
  if (!budget)
    return;
  auto sendReply = [&]() {
    size_t count = std::min(budget, c.replyLength - c.sending);
    int n = transport_->write(c.socket, work_->reply + c.sending, count);
    if (n < 0 || size_t(n) > count) {
      drop(owner_);
      return;
    }
    if (n) {
      c.last = now;
      c.sending += n;
      metrics_.sent += n;
      if (config_.bytesPerSecond)
        tokens_ -= n;
    }
    if (c.sending == c.replyLength) {
      c.markerUsed = 0;
      c.received = 0;
      c.remaining = 0;
      c.fragments = 0;
      c.sending = 0;
      c.replyLength = 0;
      owner_ = -1;
    }
  };
  if (c.replyLength) {
    sendReply();
    return;
  }
  while (budget) {
    if (!c.remaining) {
      size_t need = 4 - c.markerUsed;
      int n = transport_->read(c.socket, c.marker + c.markerUsed, std::min(need, budget));
      if (n < 0 || size_t(n) > std::min(need, budget)) {
        drop(owner_);
        return;
      }
      if (!n)
        return;
      c.last = now;
      c.markerUsed += n;
      budget -= n;
      metrics_.received += n;
      if (config_.bytesPerSecond)
        tokens_ -= n;
      if (c.markerUsed != 4)
        return;
      XdrIn mark(c.marker, 4);
      uint32_t m = mark.u32();
      c.finalFragment = m >> 31;
      c.remaining = m & 0x7fffffff;
      c.markerUsed = 0;
      if (++c.fragments > 64 || c.remaining > RecordSize - c.received) {
        ++metrics_.rejected;
        drop(owner_);
        return;
      }
      // A final zero-length fragment terminates a record; nonfinal empty fragments are bounded
      // above.
      if (!c.remaining && !c.finalFragment)
        continue;
    }
    if (c.remaining) {
      size_t want = std::min<size_t>(c.remaining, budget);
      int n = transport_->read(c.socket, work_->request + c.received, want);
      if (n < 0 || size_t(n) > want) {
        drop(owner_);
        return;
      }
      if (!n)
        return;
      c.last = now;
      c.received += n;
      c.remaining -= n;
      budget -= n;
      metrics_.received += n;
      if (config_.bytesPerSecond)
        tokens_ -= n;
    }
    if (!c.remaining && c.finalFragment) {
      size_t n = rpc(work_->request, c.received, work_->reply + 4, RecordSize, c.peer, now);
      if (!n) {
        drop(owner_);
        return;
      }
      XdrOut mark(work_->reply, 4);
      mark.u32(0x80000000u | uint32_t(n));
      c.replyLength = n + 4;
      // Use this poll's remaining network budget; do not delay a ready reply one loop.
      if (budget)
        sendReply();
      return;
    }
  }
}
size_t Server::rpc(const uint8_t *req, size_t length, uint8_t *out, size_t capacity, uint64_t peer,
                   uint32_t now) {
  if (!epoch_ || length < 24 || length > RecordSize || capacity < ReplayReplySize ||
      !ensureWorkspace())
    return 0;
  exports_->refreshGenerations();
  if (mappingRevision_ != exports_->revision()) {
    for (auto &replay : work_->replay)
      replay.used = false;
    mappingRevision_ = exports_->revision();
  }
  XdrIn header(req, length);
  uint32_t xid = header.u32();
  header.u32();
  header.u32();
  uint32_t program = header.u32();
  header.u32();
  uint32_t procedure = header.u32();
  bool mutation = program == 100003 && (procedure == 2 || procedure == 7 ||
                                        (procedure >= 8 && procedure <= 15) || procedure == 21);
  Workspace::Replay *slot = nullptr;
  uint8_t digest[32];
  if (mutation) {
    sha256(req, length, digest);
    Workspace::Replay *oldest = nullptr;
    for (auto &r : work_->replay) {
      if (r.used && uint32_t(now - r.time) >= config_.replayWindowMs)
        r.used = false;
      if (r.used && r.peer == peer && r.requestSize == length && !memcmp(r.digest, digest, 32)) {
        if (r.replySize > capacity)
          return 0;
        memcpy(out, r.reply, r.replySize);
        ++metrics_.replayHits;
        return r.replySize;
      }
      if (!r.used && !slot)
        slot = &r;
      if (r.used && (!oldest || uint32_t(now - r.time) > uint32_t(now - oldest->time)))
        oldest = &r;
    }
    if (!slot)
      slot = oldest; // Capacity-bounded DRC, oldest eviction as in ordinary NFS servers.
  }
  // Only complete accepted NFS replies enter the bounded duplicate-request cache.
  dispatchPeer_ = peer;
  size_t result = dispatch(req, length, out, capacity);
  ++metrics_.requests;
  if (slot && result >= 28 && result <= ReplayReplySize) {
    XdrIn check(out, result);
    check.pos = 20;
    if (check.u32() == 0 && check.u32() != Jukebox) {
      slot->peer = peer;
      slot->time = now;
      slot->xid = xid;
      slot->requestSize = length;
      memcpy(slot->digest, digest, 32);
      slot->replySize = result;
      memcpy(slot->reply, out, result);
      slot->used = true;
    }
  }
  return result;
}
size_t Server::dispatch(const uint8_t *req, size_t length, uint8_t *out, size_t capacity) {
  XdrIn x(req, length);
  XdrOut y(out, capacity);
  uint32_t xid = x.u32();
  uint32_t kind = x.u32(), rpcVersion = x.u32();
  uint32_t program = x.u32(), version = x.u32(), proc = x.u32();
  y.u32(xid);
  y.u32(1);
  if (kind != 0)
    return 0;
  if (rpcVersion != 2) {
    y.u32(1);
    y.u32(0);
    y.u32(2);
    y.u32(2);
    return y.pos;
  }
  if (!auth(x, true) || !auth(x, false)) {
    y.u32(1);
    y.u32(1);
    y.u32(1);
    return y.pos;
  }
  y.u32(0);
  y.u32(0);
  y.u32(0);
  size_t accepted = y.pos;
  y.u32(0);
  auto rpcError = [&](uint32_t code) -> size_t {
    y.pos = accepted;
    y.valid = true;
    y.u32(code);
    return y.pos;
  };
  if (program != 100003 && program != 100005 && !(program == 100000 && config_.portmapper))
    return rpcError(1);
  uint32_t wanted = program == 100000 ? 2 : 3;
  if (version != wanted) {
    y.pos = accepted;
    y.u32(2);
    y.u32(wanted);
    y.u32(wanted);
    return y.pos;
  }
  if (program == 100000) {
    if (proc == 0) {
      if (!x.done())
        return rpcError(4);
      return y.pos;
    }
    if (proc == 3) {
      uint32_t p = x.u32(), v = x.u32(), proto = x.u32();
      x.u32();
      if (!x.done())
        return rpcError(4);
      y.u32((p == 100003 || p == 100005) && v == 3 && proto == 6 ? config_.port : 0);
      return y.pos;
    }
    if (proc == 4) {
      if (!x.done())
        return rpcError(4);
      for (uint32_t p : {100003u, 100005u}) {
        y.u32(1);
        y.u32(p);
        y.u32(3);
        y.u32(6);
        y.u32(config_.port);
      }
      y.u32(0);
      return y.pos;
    }
    return rpcError(3);
  }
  if (program == 100005) {
    if (proc == 1 || proc == 3) {
      size_t pathLength;
      const uint8_t *path = x.opaque(pathLength, 1024);
      if (!x.done() || !path || memchr(path, 0, pathLength))
        return rpcError(4);
      ExportMap::Mapping *mapping = nullptr;
      for (size_t i = 0; i < ESP32_NFS_EXPORT_COUNT; ++i) {
        auto *m = exports_->at(i);
        if (m && strlen(m->path) == pathLength && !memcmp(m->path, path, pathLength)) {
          mapping = m;
          break;
        }
      }
      if (proc == 1 && pathLength >= ExportMap::PathCapacity) {
        y.u32(NameTooLong);
        return y.pos;
      }
      if (proc == 3) {
        if (mapping && dispatchPeer_ == mapping->mountPeer)
          mapping->mounted = false;
        return y.pos;
      }
      if (!mapping) {
        y.u32(NoEnt);
        return y.pos;
      }
      Lease *bus = mapping->lease ? mapping->lease : lease_;
      if (bus && !bus->acquire())
        return rpcError(5);
      Attr a;
      Status s = mapping->vfs->getattr(mapping->vfs->root(), a);
      if (bus)
        bus->release();
      y.u32(s == Ok ? 0 : s == Access ? 13 : 5);
      if (s == Ok) {
        mapping->mounted = true;
        mapping->mountPeer = dispatchPeer_;
        handle(y, epoch_, mapping->token, mapping->vfs->root());
        y.u32(2);
        y.u32(0);
        y.u32(1);
      }
      return y.pos;
    }
    if (proc == 0 || proc == 2 || proc == 4 || proc == 5) {
      if (!x.done())
        return rpcError(4);
      for (size_t i = 0; i < ESP32_NFS_EXPORT_COUNT; ++i) {
        auto *m = exports_->at(i);
        if (!m)
          continue;
        if (proc == 2 && m->mounted) {
          char host[32];
          uint32_t ip = uint32_t(m->mountPeer);
          snprintf(host, sizeof(host), "%u.%u.%u.%u", unsigned(ip >> 24),
                   unsigned((ip >> 16) & 255), unsigned((ip >> 8) & 255), unsigned(ip & 255));
          y.u32(1);
          y.string(host);
          y.string(m->path);
        }
        if (proc == 4 && dispatchPeer_ == m->mountPeer)
          m->mounted = false;
        if (proc == 5) {
          y.u32(1);
          y.string(m->path);
          y.u32(0);
        }
      }
      if (proc == 2 || proc == 5)
        y.u32(0);
      return y.valid ? y.pos : rpcError(5);
    }
    return rpcError(3);
  }
  if (proc > 21)
    return rpcError(3);
  if (proc == 0) {
    if (!x.done())
      return rpcError(4);
    return y.pos;
  }
  uint64_t id = 0, other = 0;
  ExportMap::Mapping *mapping = nullptr, *otherMapping = nullptr;
  Status status = gethandle(x, epoch_, *exports_, mapping, id), second = Ok;
  char name[PathSize] = {}, newName[PathSize] = {};
  uint64_t offset = 0, verifier = 0, cookie = 0;
  uint32_t count = 0, stable = 0, accessMask = 0, how = 0, dirCount = 0;
  size_t payloadSize = 0;
  const uint8_t *payload = nullptr;
  SetAttr settings;
  bool guard = false;
  Time guardTime;
  switch (proc) {
  case 2:
    setattrs(x, settings);
    guard = x.boolean();
    if (guard) {
      guardTime.seconds = x.u32();
      guardTime.nanos = x.u32();
      if (guardTime.nanos >= 1000000000)
        x.valid = false;
    }
    break;
  case 3:
  case 12:
  case 13:
    x.string(name, sizeof(name));
    break;
  case 4:
    accessMask = x.u32();
    break;
  case 6:
    offset = x.u64();
    count = x.u32();
    break;
  case 7:
    offset = x.u64();
    count = x.u32();
    stable = x.u32();
    payload = x.opaque(payloadSize, BlockSize);
    if (stable > 2 || count != payloadSize || count > config_.blockSize)
      x.valid = false;
    break;
  case 8:
    x.string(name, sizeof(name));
    how = x.u32();
    if (how > 2)
      x.valid = false;
    if (how == 2)
      verifier = x.u64();
    else
      setattrs(x, settings);
    break;
  case 9:
    x.string(name, sizeof(name));
    setattrs(x, settings);
    break;
  case 10:
    x.string(name, sizeof(name));
    setattrs(x, settings);
    {
      size_t n;
      x.opaque(n, 1024);
    }
    break;
  case 11:
    x.string(name, sizeof(name));
    how = x.u32();
    if (how == 3 || how == 4) {
      setattrs(x, settings);
      x.u32();
      x.u32();
    } else if (how == 6 || how == 7)
      setattrs(x, settings);
    break;
  case 14:
    x.string(name, sizeof(name));
    second = gethandle(x, epoch_, *exports_, otherMapping, other);
    x.string(newName, sizeof(newName));
    break;
  case 15:
    second = gethandle(x, epoch_, *exports_, otherMapping, other);
    x.string(name, sizeof(name));
    break;
  case 16:
  case 17:
    cookie = x.u64();
    verifier = x.u64();
    if (proc == 17)
      dirCount = x.u32();
    count = x.u32();
    break;
  case 21:
    offset = x.u64();
    count = x.u32();
    if (count && offset > UINT64_MAX - count)
      x.valid = false;
    break;
  default:
    break;
  }
  if (!x.done())
    return rpcError(4);
  if (status == Ok && second != Ok)
    status = second;
  Vfs *vfs = mapping ? mapping->vfs : nullptr;
  Vfs *otherVfs = otherMapping ? otherMapping->vfs : nullptr;
  uint64_t volume = mapping ? mapping->token : 0,
           otherVolume = otherMapping ? otherMapping->token : 0;
  bool crossVolume = otherMapping && mapping && otherMapping != mapping;
  if (status == Ok && crossVolume)
    status = Xdev;
  struct BusGuard {
    Lease *bus;
    bool held;
    ~BusGuard() {
      if (held)
        bus->release();
    }
  };
  Lease *bus = mapping && mapping->lease ? mapping->lease : lease_;
  BusGuard busGuard{bus, false};
  if (status == Ok && bus) {
    if (!bus->acquire())
      return rpcError(5);
    busGuard.held = true;
  }
  bool readOnly = config_.readOnly || (mapping && mapping->readOnly) || (vfs && !vfs->writable());

  Attr before, otherBefore;
  if (status == Ok)
    status = vfs->getattr(id, before);
  bool hasBefore = status == Ok;
  bool hasOther = status == Ok && !crossVolume && otherVfs && other &&
                  otherVfs->getattr(other, otherBefore) == Ok;
  bool mutating = proc == 2 || proc == 7 || (proc >= 8 && proc <= 15) || proc == 21;
  if (status == Ok && mutating && readOnly && proc != 21)
    status = ReadOnly;
  auto putStatus = [&]() { y.u32(status); };
  auto putWcc = [&]() {
    wcc(y, crossVolume ? nullptr : vfs, id, hasBefore ? &before : nullptr, volume);
  };
  switch (proc) {
  case 1: {
    Attr a;
    if (status == Ok)
      status = vfs->getattr(id, a);
    putStatus();
    if (status == Ok)
      attr(y, a, volume);
    break;
  }
  case 2:
    if (status == Ok && guard &&
        (!hasBefore || before.ctime.seconds != guardTime.seconds ||
         before.ctime.nanos != guardTime.nanos))
      status = NotSync;
    if (status == Ok)
      status = vfs->setattr(id, settings);
    if (status == Ok)
      status = vfs->sync(id);
    putStatus();
    putWcc();
    break;
  case 3: {
    uint64_t found = 0;
    if (status == Ok)
      status = vfs->lookup(id, name, found);
    putStatus();
    if (status == Ok) {
      handle(y, epoch_, volume, found);
      post(y, vfs, found, volume);
    }
    post(y, crossVolume ? nullptr : vfs, id, volume);
    break;
  }
  case 4:
    if (status == Ok && !hasBefore)
      status = vfs->getattr(id, before);
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    if (status == Ok) {
      uint32_t allowed = before.type == 2 ? 0x1f : 0x2d;
      if (readOnly)
        allowed &= ~uint32_t(4 | 8 | 16);
      y.u32(accessMask & allowed);
    }
    break;
  case 5:
    if (status == Ok)
      status = NotSupported;
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    break;
  case 6: {
    size_t n = std::min<uint32_t>(count, config_.blockSize);
    const size_t requested = n;
    // READ data is produced directly in the final reply buffer, not a second stack buffer.
    size_t dataAt = y.pos + 4 + 4 + 84 + 4 + 4 + 4;
    if (dataAt + n + 3 > capacity)
      return rpcError(5);
    if (status == Ok)
      status = vfs->read(id, offset, out + dataAt, n);
    if (status == Ok && n > requested)
      return rpcError(5);
    putStatus();
    Attr a;
    bool got = vfs && vfs->getattr(id, a) == Ok;
    y.u32(got);
    if (got)
      attr(y, a, volume);
    if (status == Ok) {
      y.u32(n);
      y.u32(got && (offset >= a.size || n >= a.size - offset));
      y.u32(n);
      if (y.pos != dataAt) {
        memmove(out + y.pos, out + dataAt, n);
      }
      size_t padded = (n + 3) & ~size_t(3);
      if (padded > n)
        memset(out + y.pos + n, 0, padded - n);
      y.pos += padded;
    }
    break;
  }
  case 7: {
    size_t n = payloadSize;
    if (status == Ok && stable && !vfs->stableStorage())
      status = NotSupported;
    bool deferred = status == Ok && stable == 0 && vfs->bufferedWrites();
    if (status == Ok)
      status = deferred ? vfs->writeBuffered(id, offset, payload, n)
                        : vfs->write(id, offset, payload, n);
    if (status == Ok && (n > payloadSize || (payloadSize && !n)))
      status = Io;
    // Durable backends upgrade to FILE_SYNC. Volatile RAM explicitly returns UNSTABLE.
    if (status == Ok && vfs->stableStorage() && !deferred)
      status = vfs->sync(id);
    putStatus();
    putWcc();
    if (status == Ok) {
      y.u32(n);
      y.u32(vfs->stableStorage() && !deferred ? 2 : 0);
      y.u64(epoch_ ^ volume);
    }
    break;
  }
  case 8:
  case 9: {
    uint64_t found = 0;
    if (status == Ok)
      status = vfs->create(id, name, proc == 9, how, verifier, settings, found);
    putStatus();
    if (status == Ok) {
      y.u32(1);
      handle(y, epoch_, volume, found);
      post(y, vfs, found, volume);
    }
    putWcc();
    break;
  }
  case 10:
  case 11:
    if (status == Ok)
      status = NotSupported;
    putStatus();
    putWcc();
    break;
  case 12:
  case 13:
    if (status == Ok)
      status = vfs->remove(id, name, proc == 13);
    putStatus();
    putWcc();
    break;
  case 14:
    if (status == Ok)
      status = vfs->rename(id, name, other, newName);
    putStatus();
    putWcc();
    wcc(y, crossVolume ? nullptr : otherVfs, other, hasOther ? &otherBefore : nullptr, otherVolume);
    break;
  case 15:
    if (status == Ok)
      status = NotSupported;
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    wcc(y, crossVolume ? nullptr : otherVfs, other, hasOther ? &otherBefore : nullptr, otherVolume);
    break;
  case 16:
  case 17: {
    uint64_t actual = 0;
    if (status == Ok)
      status = vfs->directoryVerifier(id, actual);
    if (status == Ok && cookie && verifier != actual)
      status = BadCookie;
    if (status == Ok && count < 108)
      status = TooSmall;
    size_t statusAt = y.pos;
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    if (status != Ok)
      break;
    y.u64(actual);
    size_t base = y.pos, dirUsed = 0, entries = 0;
    bool eof = false;
    // Count/maxcount includes NFS result (not RPC header). dircount counts basic entry data.
    size_t limit = std::min<size_t>(capacity, statusAt + count);
    while (entries < config_.directoryEntriesPerRpc) {
      Entry entry;
      status = proc == 16 ? vfs->readdirNames(id, cookie, actual, entry, eof)
                          : vfs->readdir(id, cookie, actual, entry, eof);
      if (status != Ok || eof)
        break;
      size_t nameBytes = (strlen(entry.name) + 3) & ~size_t(3), basic = 4 + 8 + 4 + nameBytes + 8;
      size_t needed = basic + (proc == 17 ? 4 + 84 + 4 + 4 + 24 : 0);
      if (y.pos + needed + 8 > limit || (proc == 17 && dirUsed + basic > dirCount)) {
        if (!entries)
          status = TooSmall;
        break;
      }
      y.u32(1);
      y.u64(entry.id);
      y.string(entry.name);
      y.u64(entry.cookie);
      if (proc == 17) {
        y.u32(1);
        attr(y, entry.attr, volume);
        y.u32(1);
        handle(y, epoch_, volume, entry.id);
      }
      cookie = entry.cookie;
      dirUsed += basic;
      ++entries;
    }
    if (status != Ok) {
      y.pos = statusAt;
      y.valid = true;
      y.u32(status);
      post(y, crossVolume ? nullptr : vfs, id, volume);
    } else {
      y.u32(0);
      y.u32(eof);
    }
    (void)base;
    break;
  }
  case 18: {
    Space s;
    if (status == Ok)
      status = vfs->space(s);
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    if (status == Ok) {
      y.u64(s.totalBytes);
      y.u64(s.freeBytes);
      y.u64(s.freeBytes);
      y.u64(s.totalFiles);
      y.u64(s.freeFiles);
      y.u64(s.freeFiles);
      y.u32(0);
    }
    break;
  }
  case 19:
    if (status == Ok && !hasBefore)
      status = vfs->getattr(id, before);
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    if (status == Ok) {
      y.u32(config_.blockSize);
      y.u32(config_.blockSize);
      y.u32(4);
      y.u32(config_.blockSize);
      y.u32(config_.blockSize);
      y.u32(4);
      y.u32(config_.blockSize);
      y.u64(vfs->maxFileSize());
      y.u32(0);
      y.u32(1);
      y.u32(vfs->canSetTimes() ? 0x18 : 0x8);
    }
    break;
  case 20:
    if (status == Ok && !hasBefore)
      status = vfs->getattr(id, before);
    putStatus();
    post(y, crossVolume ? nullptr : vfs, id, volume);
    if (status == Ok) {
      y.u32(1);
      y.u32(PathSize - 2);
      y.u32(1);
      y.u32(1);
      y.u32(vfs->caseInsensitive());
      y.u32(1);
    }
    break;
  case 21:
    if (status == Ok && !vfs->stableStorage())
      status = NotSupported;
    if (status == Ok)
      status = vfs->sync(id);
    putStatus();
    putWcc();
    if (status == Ok)
      y.u64(epoch_ ^ volume);
    break;
  }
  return y.valid ? y.pos : rpcError(5);
}
} // namespace esp32_nfs
