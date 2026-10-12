#pragma once
// SPDX-License-Identifier: MIT
#include "PathVfs.h"
#include <new>
#include <string.h>
namespace esp32_nfs {
namespace path_detail {
inline uint32_t nameHash(uint64_t parent, const char *name, size_t length) {
  uint32_t hash = 2166136261u ^ uint32_t(parent) ^ uint32_t(parent >> 32);
  while (length--)
    hash = (hash ^ uint8_t(*name++)) * 16777619u;
  return hash;
}
} // namespace path_detail
inline void *PathVfs::Registry::allocate(size_t bytes) {
  void *p = memory_.allocate(bytes);
  if (p)
    bytes_ += Memory::charge(bytes);
  return p;
}
inline void PathVfs::Registry::release(void *p, size_t bytes) {
  if (!p)
    return;
  memory_.release(p, bytes);
  bytes_ -= Memory::charge(bytes);
}
inline PathVfs::PathVfs(PathStorage &s) : storage_(s), registry_(&ownRegistry_) {}
inline PathVfs::PathVfs(PathStorage &s, Registry &r) : storage_(s), registry_(&r) {}
inline PathVfs::~PathVfs() {
  clearNodes();
}
inline size_t PathVfs::registryBytes() const {
  return registry_->allocatedBytes();
}
inline bool PathVfs::rehash(size_t capacity) {
  if (capacity < 16 || capacity > SIZE_MAX / (2 * sizeof(Node *)))
    return false;
  auto **table = static_cast<Node **>(registry_->allocate(2 * capacity * sizeof(Node *)));
  if (!table)
    return false;
  memset(table, 0, 2 * capacity * sizeof(Node *));
  for (size_t i = 0; i < bucketCount_; ++i)
    for (Node *n = buckets_[i]; n;) {
      Node *next = n->byId;
      size_t id = n->id & (capacity - 1), name = capacity + (n->hash & (capacity - 1));
      n->byId = table[id];
      table[id] = n;
      n->byName = table[name];
      table[name] = n;
      n = next;
    }
  registry_->release(buckets_, 2 * bucketCount_ * sizeof(Node *));
  buckets_ = table;
  bucketCount_ = capacity;
  return true;
}
inline void PathVfs::trim() {
  if (!count_) {
    registry_->release(buckets_, 2 * bucketCount_ * sizeof(Node *));
    buckets_ = nullptr;
    bucketCount_ = 0;
  } else if (bucketCount_ > 16 && count_ < bucketCount_) {
    size_t capacity = 16;
    while (count_ > 4 * capacity)
      capacity *= 2;
    if (capacity < bucketCount_)
      rehash(capacity);
  }
}
inline PathVfs::Node *PathVfs::node(uint64_t id) const {
  if (!id || !bucketCount_)
    return nullptr;
  for (Node *n = buckets_[id & (bucketCount_ - 1)]; n; n = n->byId)
    if (n->id == id)
      return n;
  return nullptr;
}
inline PathVfs::Node *PathVfs::named(uint64_t parent, const char *name, size_t length) const {
  if (!bucketCount_)
    return nullptr;
  uint32_t hash = path_detail::nameHash(parent, name, length);
  for (Node *n = buckets_[bucketCount_ + (hash & (bucketCount_ - 1))]; n; n = n->byName)
    if (n->hash == hash && n->parent == parent && n->nameLength == length &&
        !memcmp(n->name(), name, length))
      return n;
  return nullptr;
}
inline PathVfs::Node *PathVfs::makeNode(uint64_t parent, const char *name, size_t length,
                                        uint64_t id) {
  auto *block = registry_->allocate(sizeof(Node) + length + 1);
  if (!block)
    return nullptr;
  auto *n = new (block) Node;
  n->id = id;
  n->parent = parent;
  n->revision = revision_;
  n->hash = path_detail::nameHash(parent, name, length);
  n->nameLength = uint16_t(length);
  memcpy(n->name(), name, length);
  n->name()[length] = 0;
  return n;
}
inline void PathVfs::link(Node *n) {
  size_t id = n->id & (bucketCount_ - 1), name = bucketCount_ + (n->hash & (bucketCount_ - 1));
  n->byId = buckets_[id];
  buckets_[id] = n;
  n->byName = buckets_[name];
  buckets_[name] = n;
  Node *parent = node(n->parent);
  if (parent && parent->children != UINT16_MAX)
    ++parent->children;
  ++count_;
  ++registry_->count_;
}
inline void PathVfs::unlink(Node *n) {
  Node **p = &buckets_[n->id & (bucketCount_ - 1)];
  while (*p != n)
    p = &(*p)->byId;
  *p = n->byId;
  p = &buckets_[bucketCount_ + (n->hash & (bucketCount_ - 1))];
  while (*p != n)
    p = &(*p)->byName;
  *p = n->byName;
  Node *parent = node(n->parent);
  if (parent && parent->children && parent->children != UINT16_MAX)
    --parent->children;
  --count_;
  --registry_->count_;
}
inline void PathVfs::destroy(Node *n) {
  registry_->release(n, sizeof(Node) + n->nameLength + 1);
}
inline void PathVfs::clearNodes() {
  for (size_t i = 0; i < bucketCount_; ++i)
    for (Node *n = buckets_[i]; n;) {
      Node *next = n->byId;
      destroy(n);
      n = next;
    }
  registry_->count_ -= count_;
  count_ = 0;
  trim();
}
inline bool PathVfs::ensure() {
  if (!rootId_)
    return false;
  if (node(rootId_))
    return true;
  if (!bucketCount_ && !rehash(16))
    return false;
  Node *root = makeNode(0, "", 0, rootId_);
  if (!root) {
    trim();
    return false;
  }
  link(root);
  return true;
}
inline Status PathVfs::path(const Node *n, char *out) const {
  if (!n)
    return Stale;
  size_t at = PathSize - 1;
  out[at] = 0;
  while (n->parent) {
    if (n->nameLength + 1 > at)
      return NameTooLong;
    at -= n->nameLength;
    memcpy(out + at, n->name(), n->nameLength);
    out[--at] = '/';
    n = node(n->parent);
    if (!n)
      return Stale;
  }
  if (n->id != rootId_)
    return Stale;
  if (at == PathSize - 1)
    out[--at] = '/';
  memmove(out, out + at, PathSize - at);
  return Ok;
}
inline bool PathVfs::descendant(const Node *n, uint64_t parent) const {
  for (size_t depth = 0; n && depth < PathSize; ++depth) {
    if (n->id == parent)
      return true;
    n = node(n->parent);
  }
  return false;
}
inline PathVfs::Node *PathVfs::find(const char *p) {
  if (!ensure() || !p || *p++ != '/')
    return nullptr;
  Node *n = node(rootId_);
  while (*p) {
    const char *end = strchr(p, '/');
    size_t length = end ? size_t(end - p) : strlen(p);
    n = named(n->id, p, length);
    if (!n || !end)
      return n;
    p = end + 1;
  }
  return n;
}
inline Status PathVfs::intern(const char *p, uint64_t &id) {
  if (!ensure())
    return Jukebox;
  Node *n = node(rootId_);
  uint64_t firstNew = 0;
  for (++p; *p;) {
    const char *end = strchr(p, '/');
    size_t length = end ? size_t(end - p) : strlen(p);
    Node *child = named(n->id, p, length);
    if (!child) {
      if (count_ >= 4 * bucketCount_ && bucketCount_ <= SIZE_MAX / 4)
        rehash(2 * bucketCount_);
      child = nextId_ == UINT64_MAX ? nullptr : makeNode(n->id, p, length, nextId_++);
      if (!child) {
        while (firstNew && n->id >= firstNew) {
          Node *parent = node(n->parent);
          unlink(n);
          destroy(n);
          n = parent;
        }
        trim();
        return Jukebox;
      }
      if (!firstNew)
        firstNew = child->id;
      link(child);
    }
    n = child;
    if (!end)
      break;
    p = end + 1;
  }
  id = n->id;
  return Ok;
}
inline void PathVfs::invalidateAll() {
  storage_.invalidateCaches();
  clearNodes();
  rootId_ = nextId_ == UINT64_MAX ? 0 : nextId_++;
  ++revision_;
}
inline Status PathVfs::endSession() {
  Status s = storage_.releaseResources();
  clearNodes();
  rootId_ = nextId_ == UINT64_MAX ? 0 : nextId_++;
  ++revision_;
  return s;
}
inline Status PathVfs::releaseCaches() {
  Status s = storage_.releaseResources();
  if (s == Ok)
    trim();
  return s;
}
inline bool PathVfs::equal(const char *a, const char *b) const {
  return !strcmp(a, b);
}
inline bool PathVfs::below(const char *p, const char *base) const {
  return name_detail::below(p, base);
}
inline Status PathVfs::resolvePath(const char *p, uint64_t &id) {
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
inline Status PathVfs::canonicalPath(uint64_t id, char *out, size_t cap) {
  Attr a;
  Status s = getattr(id, a);
  if (s != Ok)
    return s;
  char p[PathSize];
  s = path(node(id), p);
  if (s != Ok)
    return s;
  if (!out || strlen(p) >= cap)
    return NameTooLong;
  strcpy(out, p);
  return Ok;
}
inline void PathVfs::changed(Node *n) {
  if (n)
    n->revision = ++revision_;
}
inline void PathVfs::notifyDirectoryChanged(uint64_t id) {
  storage_.invalidateCaches();
  changed(node(id));
}
inline Status PathVfs::getattr(uint64_t id, Attr &a) {
  char p[PathSize];
  return attributes(id, a, p);
}
inline Status PathVfs::attributes(uint64_t id, Attr &a, char *p) {
  if (id == rootId_ && !ensure())
    return Jukebox;
  Node *n = node(id);
  Status s = path(n, p);
  if (s != Ok)
    return s;
  s = storage_.stat(p, a);
  if (s == NoEnt) {
    forget(p);
    return Stale;
  }
  if (s == Ok) {
    if (n->storageId && a.id && n->storageId != a.id) {
      forget(p);
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
inline Status PathVfs::child(uint64_t dir, const char *name, char *path) {
  Attr a;
  char parent[PathSize];
  Status s = attributes(dir, a, parent);
  if (s != Ok)
    return s;
  if (a.type != 2)
    return NotDir;
  Status valid = name_detail::component(name);
  if (valid != Ok)
    return valid;
  if (!strcmp(name, ".")) {
    strcpy(path, parent);
    return Ok;
  }
  if (!strcmp(name, "..")) {
    strcpy(path, parent);
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
  size_t len = strlen(parent), sep = len > 1 ? 1 : 0;
  if (len + sep + z >= PathSize)
    return NameTooLong;
  strcpy(path, parent);
  if (sep)
    strcat(path, "/");
  strcat(path, name);
  return Ok;
}
inline Status PathVfs::lookup(uint64_t d, const char *name, uint64_t &id) {
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
inline Status PathVfs::setattr(uint64_t id, const SetAttr &a) {
  Attr old;
  char path[PathSize];
  Status s = attributes(id, old, path);
  if (s != Ok)
    return s;
  if (a.sizeSet && old.type == 2)
    return IsDir;
  if (a.sizeSet && a.size > maxFileSize())
    return FBig;
  s = storage_.setattr(path, a);
  if (s == Ok)
    changed(node(id));
  return s;
}
inline Status PathVfs::read(uint64_t id, uint64_t off, uint8_t *p, size_t &count) {
  Attr a;
  char path[PathSize];
  Status s = attributes(id, a, path);
  if (s != Ok)
    return s;
  if (a.type != 1)
    return IsDir;
  if (off > maxFileSize())
    return FBig;
  return s == Ok ? storage_.read(path, off, p, count) : s;
}
inline Status PathVfs::write(uint64_t id, uint64_t off, const uint8_t *p, size_t &count) {
  return writeData(id, off, p, count, false);
}
inline Status PathVfs::writeBuffered(uint64_t id, uint64_t off, const uint8_t *p, size_t &count) {
  return writeData(id, off, p, count, true);
}
inline Status PathVfs::writeData(uint64_t id, uint64_t off, const uint8_t *p, size_t &count,
                                 bool buffered) {
  Attr a;
  char path[PathSize];
  Status s = attributes(id, a, path);
  if (s != Ok)
    return s;
  if (a.type != 1)
    return IsDir;
  if (off > maxFileSize() || count > maxFileSize() - off)
    return FBig;
  s = buffered ? storage_.writeBuffered(path, off, p, count) : storage_.write(path, off, p, count);
  if (s == Ok)
    changed(node(id));
  return s;
}
inline Status PathVfs::create(uint64_t d, const char *name, bool directory, uint32_t how,
                              uint64_t verifier, const SetAttr &requested, uint64_t &id) {
  SetAttr attrs = requested;
  attrs.modeSet = attrs.uidSet = attrs.gidSet = false;
  if (!name)
    return Invalid;
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
inline void PathVfs::forget(const char *p) {
  Node *root = find(p);
  if (!root)
    return;
  if (!root->children && root->id != rootId_) {
    unlink(root);
    destroy(root);
    trim();
    return;
  }
  for (size_t i = 0; i < bucketCount_; ++i)
    for (Node *n = buckets_[i]; n; n = n->byId)
      if (n->id != rootId_ && descendant(n, root->id))
        n->revision = 0;
  for (size_t i = 0; i < bucketCount_; ++i)
    for (Node *n = buckets_[i]; n;) {
      Node *next = n->byId;
      if (!n->revision) {
        unlink(n);
        destroy(n);
      }
      n = next;
    }
  trim();
}
inline Status PathVfs::remove(uint64_t d, const char *name, bool directory) {
  if (!name)
    return Invalid;
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
inline Status PathVfs::rename(uint64_t from, const char *old, uint64_t to, const char *name) {
  if (!old || !name || !strcmp(old, ".") || !strcmp(old, "..") || !strcmp(name, ".") ||
      !strcmp(name, ".."))
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
  if (replacing && below(a, b))
    return Invalid;
  uint64_t source;
  s = intern(a, source);
  if (s != Ok)
    return s;
  size_t an = strlen(a), bn = strlen(b);
  for (size_t i = 0; bn > an && i < bucketCount_; ++i)
    for (Node *n = buckets_[i]; n; n = n->byId)
      if (descendant(n, source)) {
        s = path(n, scratch);
        if (s != Ok)
          return s;
        if (bn + strlen(scratch) - an >= PathSize)
          return NameTooLong;
      }
  const char *leaf = strrchr(b, '/') + 1;
  Node *oldNode = node(source);
  Node *moved = makeNode(to, leaf, strlen(leaf), source);
  if (!moved)
    return Jukebox;
  moved->storageId = oldNode->storageId;
  moved->children = oldNode->children;
  s = storage_.rename(a, b);
  if (s != Ok) {
    destroy(moved);
    return s;
  }
  if (replacing)
    forget(b);
  unlink(oldNode);
  destroy(oldNode);
  moved->revision = ++revision_;
  link(moved);
  changed(node(from));
  changed(node(to));
  return Ok;
}
inline Status PathVfs::directoryVerifier(uint64_t d, uint64_t &v) {
  Attr a;
  Status s = getattr(d, a);
  if (s != Ok)
    return s;
  if (a.type != 2)
    return NotDir;
  v = node(d)->revision;
  return Ok;
}
inline Status PathVfs::readdir(uint64_t d, uint64_t cookie, uint64_t verifier, Entry &e,
                               bool &eof) {
  return readDirectory(d, cookie, verifier, e, eof, true);
}
inline Status PathVfs::readdirNames(uint64_t d, uint64_t cookie, uint64_t verifier, Entry &e,
                                    bool &eof) {
  return readDirectory(d, cookie, verifier, e, eof, false);
}
inline Status PathVfs::readDirectory(uint64_t d, uint64_t cookie, uint64_t verifier, Entry &e,
                                     bool &eof, bool attributes) {
  uint64_t actual;
  Status s = directoryVerifier(d, actual);
  if (s != Ok)
    return s;
  if ((cookie && actual != verifier) || cookie == UINT64_MAX)
    return BadCookie;
  char full[PathSize];
  s = path(node(d), full);
  if (s != Ok)
    return s;
  s = attributes ? storage_.entryInfo(full, cookie, e.name, sizeof(e.name), e.attr, eof)
                 : storage_.entry(full, cookie, e.name, sizeof(e.name), eof);
  if (s != Ok || eof)
    return s;
  s = storage_.validateName(e.name);
  if (s != Ok)
    return s;
  if (!strcmp(e.name, ".") || !strcmp(e.name, ".."))
    return Invalid;
  size_t length = strlen(full), sep = length > 1, n = strlen(e.name);
  if (length + sep + n >= PathSize)
    return NameTooLong;
  if (sep)
    full[length++] = '/';
  memcpy(full + length, e.name, n + 1);
  Node *existing = find(full);
  if (attributes && existing && existing->storageId && e.attr.id &&
      existing->storageId != e.attr.id)
    forget(full);
  s = intern(full, e.id);
  if (s != Ok)
    return s;
  if (attributes) {
    Node *entry = node(e.id);
    entry->storageId = e.attr.id;
    if (!storage_.canSetTimes())
      e.attr.ctime.nanos = e.attr.mtime.nanos = entry->revision % 1000000000;
    e.attr.id = e.id;
    e.attr.fsid = 1;
  }
  e.cookie = cookie + 1;
  return Ok;
}
inline Status PathVfs::sync(uint64_t id) {
  Attr a;
  char p[PathSize];
  Status s = attributes(id, a, p);
  return s == Ok ? storage_.sync(p) : s;
}
} // namespace esp32_nfs
