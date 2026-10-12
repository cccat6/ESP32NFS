// SPDX-License-Identifier: MIT
#include <assert.h>
#include <nfsc/libnfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(call)                                                                                \
  do {                                                                                             \
    if (getenv("NFS_TEST_TRACE"))                                                                  \
      fprintf(stderr, "CALL %s\n", #call);                                                         \
    int result = (call);                                                                           \
    if (result < 0) {                                                                              \
      fprintf(stderr, "%s: %s\n", #call, nfs_get_error(nfs));                                      \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)
int main(int argc, char **argv) {
  assert(argc == 4);
  size_t payload = (size_t)atoi(argv[3]);
  struct nfs_context *nfs = nfs_init_context();
  assert(nfs);
  nfs_set_version(nfs, 3);
  nfs_set_nfsport(nfs, atoi(argv[1]));
  nfs_set_mountport(nfs, atoi(argv[1]));
  nfs_set_timeout(nfs, 3000);
  nfs_set_uid(nfs, getuid());
  nfs_set_gid(nfs, getgid());
  CHECK(nfs_mount(nfs, "127.0.0.1", "/"));
  if (strcmp(argv[2], "-")) {
    struct nfs_stat_64 longName, shortName;
    CHECK(nfs_stat64(nfs, "/别名-目录/Display_中文_测试.bin", &longName));
    CHECK(nfs_stat64(nfs, argv[2], &shortName));
    assert(longName.nfs_ino == shortName.nfs_ino);
  }
  CHECK(nfs_mkdir(nfs, "/目录-É"));
  struct nfsfh *file = NULL;
  CHECK(nfs_creat(nfs, "/目录-é/混合-É😀.bin", 0644, &file));
  unsigned char data[16384], got[16384];
  for (unsigned i = 0; i < sizeof(data); ++i)
    data[i] = i * 17;
  if (getenv("NFS_TEST_TRACE"))
    fprintf(stderr, "WRITE %zu\n", sizeof(data));
  for (size_t off = 0; off < sizeof(data); off += payload) {
    int result = nfs_pwrite(nfs, file, data + off, payload, off);
    if (result != (int)payload)
      fprintf(stderr, "WRITE result %d: %s\n", result, nfs_get_error(nfs));
    assert(result == (int)payload);
  }
  for (unsigned i = 0; i < 16; ++i) {
    unsigned off = i * 131;
    unsigned char patch[73];
    memset(patch, i, sizeof(patch));
    assert(nfs_pwrite(nfs, file, patch, sizeof(patch), off) == sizeof(patch));
    memcpy(data + off, patch, sizeof(patch));
  }
  CHECK(nfs_fsync(nfs, file));
  for (size_t off = 0; off < sizeof(got); off += payload)
    assert(nfs_pread(nfs, file, got + off, payload, off) == payload);
  assert(!memcmp(data, got, sizeof(got)));
  struct nfsdir *bulk = NULL;
  CHECK(nfs_opendir(nfs, "/bulk", &bulk));
  struct nfsdirent *item;
  unsigned count = 0;
  while ((item = nfs_readdir(nfs, bulk)))
    if (item->name[0] == 'F')
      ++count;
  nfs_closedir(nfs, bulk);
  assert(count == 1100);
  for (size_t off = 0; off < sizeof(got); off += payload)
    assert(nfs_pread(nfs, file, got + off, payload, off) == payload);
  assert(!memcmp(data, got, sizeof(got)));
  CHECK(nfs_rename(nfs, "/目录-é", "/移动-目录"));
  for (size_t off = 0; off < sizeof(got); off += payload)
    assert(nfs_pread(nfs, file, got + off, payload, off) == payload);
  assert(!memcmp(data, got, sizeof(got)));
  CHECK(nfs_close(nfs, file));
  struct nfsdir *dir = NULL;
  CHECK(nfs_opendir(nfs, "/移动-目录", &dir));
  struct nfsdirent *entry;
  int found = 0;
  while ((entry = nfs_readdir(nfs, dir)))
    if (!strcmp(entry->name, "混合-É😀.bin"))
      ++found;
  nfs_closedir(nfs, dir);
  assert(found == 1);
  CHECK(nfs_truncate(nfs, "/移动-目录/混合-é😀.BIN", 9));
  struct nfs_stat_64 stat;
  CHECK(nfs_stat64(nfs, "/移动-目录/混合-é😀.BIN", &stat));
  assert(stat.nfs_size == 9);
  CHECK(nfs_unlink(nfs, "/移动-目录/混合-é😀.BIN"));
  CHECK(nfs_rmdir(nfs, "/移动-目录"));
  const char *names[] = {"/é.txt", "/é.txt"};
  for (int i = 0; i < 2; ++i) {
    CHECK(nfs_creat(nfs, names[i], 0644, &file));
    char c = 'A' + i;
    assert(nfs_pwrite(nfs, file, &c, 1, 0) == 1);
    CHECK(nfs_close(nfs, file));
  }
  for (int i = 0; i < 2; ++i) {
    CHECK(nfs_open(nfs, names[i], 0, &file));
    char c = 0;
    assert(nfs_pread(nfs, file, &c, 1, 0) == 1 && c == 'A' + i);
    CHECK(nfs_close(nfs, file));
    CHECK(nfs_unlink(nfs, names[i]));
  }
  nfs_destroy_context(nfs);
  puts("libnfs UTF-8 create, random I/O, rename, readdir, NFC/NFD and removal PASS");
  return 0;
}
