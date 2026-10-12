// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include "../Names.h"
#include <string.h>
namespace esp32_nfs {
// A storage adapter must stay inside its export root and disallow symlinks. Bounded read
// caches must release handles in invalidateCaches(). Mutations are durable on success except
// the explicitly advertised writeBuffered path, which requires sync before stable acknowledgment.
class PathStorage {
public:
  virtual ~PathStorage() {}
  virtual uint64_t generation() const {
    return 1;
  }
  virtual Status stat(const char *, Attr &) = 0;
  virtual bool sameName(const char *a, const char *b) const {
    return !strcmp(a, b);
  }
  virtual Status validateName(const char *name) const {
    return name_detail::component(name);
  }
  // Resolve existing objects to stored, absolute names. Alias-providing backends
  // override this method; output may alias input. This operation is read-only.
  virtual Status resolve(const char *p, char *canonical, size_t capacity, Attr &a) {
    Status s = name_detail::path(p);
    if (s != Ok)
      return s;
    if (caseInsensitive())
      return NotSupported;
    if (!canonical || strlen(p) >= capacity)
      return NameTooLong;
    s = stat(p, a);
    if (s == Ok)
      memmove(canonical, p, strlen(p) + 1);
    return s;
  }
  virtual Status setattr(const char *, const SetAttr &) = 0;
  virtual Status read(const char *, uint64_t, uint8_t *, size_t &) = 0;
  virtual Status write(const char *, uint64_t, const uint8_t *, size_t &) = 0;
  virtual bool bufferedWrites() const {
    return false;
  }
  virtual Status writeBuffered(const char *p, uint64_t off, const uint8_t *data, size_t &n) {
    return write(p, off, data, n);
  }
  virtual Status create(const char *, bool) = 0;
  // Durable verifier is required for NFS EXCLUSIVE; unsupported adapters must reject it.
  virtual Status createExclusive(const char *, uint64_t) {
    return NotSupported;
  }
  virtual Status remove(const char *, bool) = 0;
  virtual Status rename(const char *, const char *) = 0; // atomic replace, or NOTSUPP if impossible
  virtual Status entry(const char *, uint64_t index, char *name, size_t cap, bool &eof) = 0;
  // Return attributes from the same enumeration when available, avoiding repeated opens.
  virtual Status entryInfo(const char *p, uint64_t index, char *name, size_t cap, Attr &a,
                           bool &eof) {
    Status s = entry(p, index, name, cap, eof);
    if (s != Ok || eof)
      return s;
    if (!*name || strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") ||
        !strcmp(name, ".."))
      return Invalid;
    size_t n = strlen(p), z = strlen(name), sep = n > 1;
    if (n + sep + z >= PathSize)
      return NameTooLong;
    char path[PathSize];
    memcpy(path, p, n);
    if (sep)
      path[n++] = '/';
    memcpy(path + n, name, z + 1);
    return stat(path, a);
  }
  // Required before external mutation/remount and when ending a VFS session.
  virtual void invalidateCaches() {}
  virtual Status sync(const char *) = 0;
  virtual Status space(Space &) = 0;
  virtual uint64_t maxFileSize() const = 0;
  virtual bool caseInsensitive() const {
    return false;
  }
  virtual bool canSetTimes() const {
    return false;
  }
  virtual bool writable() const {
    return true;
  }
};
class PathVfs : public Vfs {
private:
  struct Node {
    const PathVfs *owner = nullptr;
    uint64_t id = 0, revision = 1, storageId = 0;
    char path[PathSize] = {};
  };

public:
  class Registry {
    friend class PathVfs;
    Node *nodes_ = nullptr;

  public:
    Registry() = default;
    ~Registry() {
      delete[] nodes_;
    }
    Registry(const Registry &) = delete;
    Registry &operator=(const Registry &) = delete;
    size_t allocatedBytes() const {
      return nodes_ ? sizeof(Node) * ESP32_NFS_HANDLE_COUNT : 0;
    }
  };

  explicit PathVfs(PathStorage &);
  PathVfs(PathStorage &, Registry &sharedRegistry);
  ~PathVfs();
  PathVfs(const PathVfs &) = delete;
  PathVfs &operator=(const PathVfs &) = delete;
  // External namespace changes/remounts require invalidateAll before access resumes.
  // notifyDirectoryChanged refreshes cookies/caches while existing paths remain valid.
  void invalidateAll();
  void endSession() override {
    invalidateAll();
  }
  void notifyDirectoryChanged(uint64_t dir);
  // Use (volume/session, id) for leases. IDs are shared by aliases and survive
  // library-managed rename. Hold the same volume lease through resolve/authorize/I/O.
  Status resolvePath(const char *absolute, uint64_t &id);
  Status canonicalPath(uint64_t id, char *output, size_t capacity);
  size_t registryBytes() const;
  uint64_t generation() const override {
    return storage_.generation();
  }
  uint64_t root() const override {
    return rootId_;
  }
  Status getattr(uint64_t, Attr &) override;
  Status lookup(uint64_t, const char *, uint64_t &) override;
  Status setattr(uint64_t, const SetAttr &) override;
  Status read(uint64_t, uint64_t, uint8_t *, size_t &) override;
  Status write(uint64_t, uint64_t, const uint8_t *, size_t &) override;
  bool bufferedWrites() const override {
    return storage_.bufferedWrites();
  }
  Status writeBuffered(uint64_t, uint64_t, const uint8_t *, size_t &) override;
  Status readdirNames(uint64_t, uint64_t, uint64_t, Entry &, bool &) override;
  Status create(uint64_t, const char *, bool, uint32_t, uint64_t, const SetAttr &,
                uint64_t &) override;
  Status remove(uint64_t, const char *, bool) override;
  Status rename(uint64_t, const char *, uint64_t, const char *) override;
  Status readdir(uint64_t, uint64_t, uint64_t, Entry &, bool &) override;
  Status directoryVerifier(uint64_t, uint64_t &) override;
  Status sync(uint64_t) override;
  Status space(Space &s) override {
    return storage_.space(s);
  }
  uint64_t maxFileSize() const override {
    return storage_.maxFileSize();
  }
  bool writable() const override {
    return storage_.writable();
  }
  bool caseInsensitive() const override {
    return storage_.caseInsensitive();
  }
  bool canSetTimes() const override {
    return storage_.canSetTimes();
  }

private:
  PathStorage &storage_;
  Registry ownRegistry_;
  Registry *registry_;
  uint64_t nextId_ = 2, rootId_ = 1, revision_ = 1;
  bool ensure();
  Node *node(uint64_t);
  Node *find(const char *);
  Status intern(const char *, uint64_t &);
  Status child(uint64_t, const char *, char *);
  bool equal(const char *, const char *) const;
  bool below(const char *, const char *) const;
  void forget(const char *);
  void changed(Node *);
  Status writeData(uint64_t, uint64_t, const uint8_t *, size_t &, bool);
};
} // namespace esp32_nfs
