// SPDX-License-Identifier: MIT
#include "PathVfs.h"
#include <new>
#include <string.h>
namespace esp32_nfs {
PathVfs::PathVfs(PathStorage &s) : storage_(s), registry_(&ownRegistry_) {}
PathVfs::PathVfs(PathStorage &s, Registry &r) : storage_(s), registry_(&r) {}
PathVfs::~PathVfs() {
  invalidateAll();
}
size_t PathVfs::registryBytes() const {
  return registry_->allocatedBytes();
}
bool PathVfs::ensure() {
  if (!rootId_)
    return false;
  if (!registry_->nodes_)
    registry_->nodes_ = new (std::nothrow) Node[ESP32_NFS_HANDLE_COUNT];
  if (!registry_->nodes_)
    return false;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id == rootId_)
      return true;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (!registry_->nodes_[i].id) {
      auto &root = registry_->nodes_[i];
      root = Node();
      root.owner = this;
      root.id = rootId_;
      strcpy(root.path, "/");
      return true;
    }
  return false;
}
void PathVfs::invalidateAll() {
  storage_.invalidateCaches();
  if (registry_->nodes_) {
    bool remaining = false;
    for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i) {
      auto &n = registry_->nodes_[i];
      if (n.owner == this)
        n = Node();
      remaining |= n.id != 0;
    }
    if (!remaining) {
      delete[] registry_->nodes_;
      registry_->nodes_ = nullptr;
    }
  }
  if (nextId_ == UINT64_MAX) {
    rootId_ = 0;
    return;
  }
  rootId_ = nextId_++;
  ++revision_;
}
bool PathVfs::equal(const char *a, const char *b) const {
  return !strcmp(a, b);
}
bool PathVfs::below(const char *p, const char *base) const {
  return name_detail::below(p, base);
}
Status PathVfs::resolvePath(const char *p, uint64_t &id) {
  char canonical[PathSize];
  Attr a;
  Status s = storage_.resolve(p, canonical, sizeof(canonical), a);
  if (s != Ok)
    return s;
  Node *existing = find(canonical);
  if (existing && existing->storageId && a.id && existing->storageId != a.id)
    forget(canonical);
  s = intern(canonical, id);
  if (s == Ok)
    node(id)->storageId = a.id;
  return s;
}
Status PathVfs::canonicalPath(uint64_t id, char *out, size_t cap) {
  Attr a;
  Status s = getattr(id, a);
  if (s != Ok)
    return s;
  const char *p = node(id)->path;
  if (!out || strlen(p) >= cap)
    return NameTooLong;
  memcpy(out, p, strlen(p) + 1);
  return Ok;
}
PathVfs::Node *PathVfs::node(uint64_t id) {
  if (!id)
    return nullptr;
  if (!ensure())
    return nullptr;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id == id)
      return registry_->nodes_ + i;
  return nullptr;
}
PathVfs::Node *PathVfs::find(const char *path) {
  if (!ensure())
    return nullptr;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id &&
        equal(registry_->nodes_[i].path, path))
      return registry_->nodes_ + i;
  return nullptr;
}
Status PathVfs::intern(const char *path, uint64_t &id) {
  Node *n = find(path);
  if (n) {
    id = n->id;
    return Ok;
  }
  if (!registry_->nodes_ || nextId_ == UINT64_MAX)
    return Jukebox;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (!registry_->nodes_[i].id) {
      registry_->nodes_[i] = Node();
      registry_->nodes_[i].id = nextId_++;
      registry_->nodes_[i].owner = this;
      strcpy(registry_->nodes_[i].path, path);
      id = registry_->nodes_[i].id;
      return Ok;
    }
  return Jukebox;
}
void PathVfs::changed(Node *n) {
  if (n)
    n->revision = ++revision_;
}
void PathVfs::notifyDirectoryChanged(uint64_t id) {
  storage_.invalidateCaches();
  changed(node(id));
}
Status PathVfs::getattr(uint64_t id, Attr &a) {
  Node *n = node(id);
  if (!n)
    return id == rootId_ ? Jukebox : registry_->nodes_ ? Stale : Jukebox;
  Status s = storage_.stat(n->path, a);
  if (s == NoEnt) {
    forget(n->path);
    return Stale;
  }
  if (s == Ok) {
    if (n->storageId && a.id && n->storageId != a.id) {
      forget(n->path);
      return Stale;
    }
    n->storageId = a.id;
    if (!storage_.canSetTimes())
      a.ctime.nanos = a.mtime.nanos = n->revision % 1000000000;
    a.id = id;
    a.fsid = 1;
  }
  return s;
}
Status PathVfs::child(uint64_t dir, const char *name, char *path) {
  Attr a;
  Status s = getattr(dir, a);
  if (s != Ok)
    return s;
  if (a.type != 2)
    return NotDir;
  Status valid = name_detail::component(name);
  if (valid != Ok)
    return valid;
  Node *n = node(dir);
  if (!strcmp(name, ".")) {
    strcpy(path, n->path);
    return Ok;
  }
  if (!strcmp(name, "..")) {
    strcpy(path, n->path);
    char *p = strrchr(path, '/');
    if (p == path)
      path[1] = 0;
    else
      *p = 0;
    return Ok;
  }
  s = storage_.validateName(name);
  if (s != Ok)
    return s;
  size_t z = strlen(name);
  size_t len = strlen(n->path), sep = len > 1 ? 1 : 0;
  if (len + sep + z >= PathSize)
    return NameTooLong;
  strcpy(path, n->path);
  if (sep)
    strcat(path, "/");
  strcat(path, name);
  return Ok;
}
Status PathVfs::lookup(uint64_t d, const char *name, uint64_t &id) {
  char p[PathSize];
  Status s = child(d, name, p);
  if (s != Ok)
    return s;
  Attr a;
  s = storage_.resolve(p, p, sizeof(p), a);
  if (s != Ok)
    return s;
  Node *existing = find(p);
  if (existing && existing->storageId && a.id && existing->storageId != a.id)
    forget(p);
  s = intern(p, id);
  if (s == Ok)
    node(id)->storageId = a.id;
  return s;
}
Status PathVfs::setattr(uint64_t id, const SetAttr &a) {
  Attr old;
  Status s = getattr(id, old);
  if (s != Ok)
    return s;
  if (a.sizeSet && old.type == 2)
    return IsDir;
  if (a.sizeSet && a.size > maxFileSize())
    return FBig;
  s = storage_.setattr(node(id)->path, a);
  if (s == Ok)
    changed(node(id));
  return s;
}
Status PathVfs::read(uint64_t id, uint64_t off, uint8_t *p, size_t &count) {
  Attr a;
  Status s = getattr(id, a);
  if (s != Ok)
    return s;
  if (a.type != 1)
    return IsDir;
  if (off > maxFileSize())
    return FBig;
  return storage_.read(node(id)->path, off, p, count);
}
Status PathVfs::write(uint64_t id, uint64_t off, const uint8_t *p, size_t &count) {
  return writeData(id, off, p, count, false);
}
Status PathVfs::writeBuffered(uint64_t id, uint64_t off, const uint8_t *p, size_t &count) {
  return writeData(id, off, p, count, true);
}
Status PathVfs::writeData(uint64_t id, uint64_t off, const uint8_t *p, size_t &count,
                          bool buffered) {
  Attr a;
  Status s = getattr(id, a);
  if (s != Ok)
    return s;
  if (a.type != 1)
    return IsDir;
  if (off > maxFileSize() || count > maxFileSize() - off)
    return FBig;
  s = buffered ? storage_.writeBuffered(node(id)->path, off, p, count)
               : storage_.write(node(id)->path, off, p, count);
  if (s == Ok)
    changed(node(id));
  return s;
}
Status PathVfs::create(uint64_t d, const char *name, bool directory, uint32_t how,
                       uint64_t verifier, const SetAttr &requested, uint64_t &id) {
  SetAttr attrs = requested;
  attrs.modeSet = attrs.uidSet = attrs.gidSet = false;
  if (!strcmp(name, ".") || !strcmp(name, ".."))
    return Exist;
  char p[PathSize];
  Status s = child(d, name, p);
  if (s != Ok)
    return s;
  Attr a;
  s = storage_.resolve(p, p, sizeof(p), a);
  if (s == Ok) {
    if (how == 2) {
      s = storage_.createExclusive(p, verifier);
      return s == Ok ? intern(p, id) : s;
    }
    if (how == 1 || directory)
      return Exist;
    if (a.type == 2)
      return IsDir;
    s = intern(p, id);
    return s == Ok ? setattr(id, attrs) : s;
  }
  if (s != NoEnt)
    return s;
  s = child(d, name, p);
  if (s != Ok)
    return s;
  // Reserve identity before mutating storage, so capacity exhaustion cannot orphan a CREATE result.
  s = intern(p, id);
  if (s != Ok)
    return s;
  s = how == 2 ? storage_.createExclusive(p, verifier) : storage_.create(p, directory);
  if (s != Ok) {
    forget(p);
    return s;
  }
  changed(node(d));
  if (how != 2) {
    s = setattr(id, attrs);
    if (s != Ok)
      return s;
  }
  return Ok; // PathStorage mutations already report durable completion.
}
void PathVfs::forget(const char *p) {
  char copy[PathSize];
  strcpy(copy, p);
  if (!registry_->nodes_)
    return;
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id != rootId_ &&
        registry_->nodes_[i].id &&
        (equal(registry_->nodes_[i].path, copy) || below(registry_->nodes_[i].path, copy)))
      registry_->nodes_[i] = Node();
}
Status PathVfs::remove(uint64_t d, const char *name, bool directory) {
  if (!strcmp(name, ".") || !strcmp(name, ".."))
    return Invalid;
  char p[PathSize];
  Status s = child(d, name, p);
  if (s != Ok)
    return s;
  Attr a;
  s = storage_.resolve(p, p, sizeof(p), a);
  if (s != Ok)
    return s;
  if (directory != (a.type == 2))
    return directory ? NotDir : IsDir;
  s = storage_.remove(p, directory);
  if (s == Ok) {
    forget(p);
    changed(node(d));
  }
  return s;
}
Status PathVfs::rename(uint64_t from, const char *old, uint64_t to, const char *name) {
  if (!strcmp(old, ".") || !strcmp(old, "..") || !strcmp(name, ".") || !strcmp(name, ".."))
    return Invalid;
  char a[PathSize], b[PathSize];
  Status s = child(from, old, a);
  if (s != Ok)
    return s;
  s = child(to, name, b);
  if (s != Ok)
    return s;
  Attr attr;
  s = storage_.resolve(a, a, sizeof(a), attr);
  if (s != Ok)
    return s;
  char scratch[PathSize];
  Status destination = storage_.resolve(b, scratch, sizeof(scratch), attr);
  if (destination != Ok && destination != NoEnt)
    return destination;
  bool replacing = destination == Ok && !equal(a, scratch);
  if (destination == Ok && !replacing && !storage_.sameName(a, b))
    return Ok;
  if (replacing)
    strcpy(b, scratch);
  if (!strcmp(a, b))
    return Ok;
  if (below(b, a))
    return Invalid;
  size_t an = strlen(a), bn = strlen(b);
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id &&
        below(registry_->nodes_[i].path, a) &&
        bn + strlen(registry_->nodes_[i].path) - an >= PathSize)
      return NameTooLong;
  s = storage_.rename(a, b);
  if (s != Ok)
    return s;
  if (replacing)
    forget(b);
  for (size_t i = 0; i < ESP32_NFS_HANDLE_COUNT; ++i)
    if (registry_->nodes_[i].owner == this && registry_->nodes_[i].id &&
        (equal(registry_->nodes_[i].path, a) || below(registry_->nodes_[i].path, a))) {
      strcpy(scratch, b);
      strcat(scratch, registry_->nodes_[i].path + an);
      strcpy(registry_->nodes_[i].path, scratch);
      changed(registry_->nodes_ + i);
    }
  changed(node(from));
  changed(node(to));
  return Ok;
}
Status PathVfs::directoryVerifier(uint64_t d, uint64_t &v) {
  Attr a;
  Status s = getattr(d, a);
  if (s != Ok)
    return s;
  if (a.type != 2)
    return NotDir;
  v = node(d)->revision;
  return Ok;
}
Status PathVfs::readdir(uint64_t d, uint64_t cookie, uint64_t v, Entry &e, bool &eof) {
  uint64_t actual;
  Status s = directoryVerifier(d, actual);
  if (s != Ok)
    return s;
  if (cookie && actual != v)
    return BadCookie;
  if (cookie == UINT64_MAX)
    return BadCookie;
  Node *n = node(d);
  s = storage_.entryInfo(n->path, cookie, e.name, sizeof(e.name), e.attr, eof);
  if (s != Ok || eof)
    return s;
  char path[PathSize];
  s = child(d, e.name, path);
  if (s != Ok)
    return s;
  Node *existing = find(path);
  if (existing && existing->storageId && e.attr.id && existing->storageId != e.attr.id)
    forget(path);
  s = intern(path, e.id);
  if (s != Ok)
    return s;
  Node *entryNode = node(e.id);
  entryNode->storageId = e.attr.id;
  if (!storage_.canSetTimes())
    e.attr.ctime.nanos = e.attr.mtime.nanos = entryNode->revision % 1000000000;
  e.attr.id = e.id;
  e.attr.fsid = 1;
  e.cookie = cookie + 1;
  return Ok;
}
Status PathVfs::readdirNames(uint64_t d, uint64_t cookie, uint64_t ver, Entry &e, bool &eof) {
  uint64_t actual;
  Status s = directoryVerifier(d, actual);
  if (s != Ok)
    return s;
  if (cookie && actual != ver)
    return BadCookie;
  if (cookie == UINT64_MAX)
    return BadCookie;
  s = storage_.entry(node(d)->path, cookie, e.name, sizeof(e.name), eof);
  if (s != Ok || eof)
    return s;
  char p[PathSize];
  s = child(d, e.name, p);
  if (s != Ok)
    return s;
  s = intern(p, e.id);
  if (s == Ok)
    e.cookie = cookie + 1;
  return s;
}
Status PathVfs::sync(uint64_t id) {
  Attr a;
  Status s = getattr(id, a);
  return s == Ok ? storage_.sync(node(id)->path) : s;
}
} // namespace esp32_nfs
