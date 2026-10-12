#include "fixture.h"
static_assert(USE_UTF8_LONG_NAMES == 0, "Test the installed SdFat default configuration");
static uint64_t create(PathVfs &fs, uint64_t dir, const char *name, bool directory = false,
                       uint32_t how = 0, uint64_t verifier = 0) {
  uint64_t id = 0;
  Status s = fs.create(dir, name, directory, how, verifier, SetAttr(), id);
  if (s != Ok)
    fprintf(stderr, "create %s status=%u\n", name, unsigned(s));
  assert(s == Ok);
  return id;
}
static uint64_t lookup(PathVfs &fs, uint64_t dir, const char *name) {
  uint64_t id = 0;
  Status s = fs.lookup(dir, name, id);
  if (s != Ok)
    fprintf(stderr, "lookup %s status=%u\n", name, unsigned(s));
  assert(s == Ok);
  return id;
}
static void path(PathVfs &fs, uint64_t id, const char *expected) {
  char canonical[PathSize];
  assert(fs.canonicalPath(id, canonical, sizeof(canonical)) == Ok);
  if (strcmp(canonical, expected))
    fprintf(stderr, "path %s != %s\n", canonical, expected);
  assert(!strcmp(canonical, expected));
}
bool otherOpenRoot();
int checks() {
  {
    Fixture f(false, false);
    f.volume.chvol();
    assert(otherOpenRoot());
    f.volume.end();
    assert(!otherOpenRoot());
    assert(f.volume.begin(&f.disk, true, 1));
    assert(otherOpenRoot());
  }
  assert(!otherOpenRoot());
  {
    // A damaged LFN chain reports an I/O error during lookup and enumeration.
    Fixture f(false, false);
    assert(f.volume.mkdir("/damaged", false));
    auto file = f.volume.open("/damaged/Damaged_中文.txt", O_CREAT | O_RDWR);
    assert(file && file.dirIndex() > 0);
    auto root = f.volume.open("/damaged", O_RDONLY);
    uint32_t preceding = file.dirIndex();
    uint32_t sector = root.firstSector() + preceding / 16;
    uint32_t offset = (preceding % 16) * 32;
    file.close();
    root.close();
    f.volume.end();
    uint8_t bytes[512];
    assert(f.disk.readSector(sector, bytes));
    assert(bytes[offset + 11] != 0x0f);
    bytes[offset] ^= 1;
    assert(f.disk.writeSector(sector, bytes) && f.disk.syncDevice());
    assert(f.volume.begin(&f.disk, false, 1));
    Attr a;
    char name[PathSize];
    bool eof = false;
    assert(f.backend.resolve("/damaged/Damaged_中文.txt", name, sizeof(name), a) == Io);
    assert(f.backend.entry("/damaged", 0, name, sizeof(name), eof) == Io);
  }

  for (bool exfat : {false, true})
    for (bool transactional : {false, true}) {
      Fixture f(exfat, transactional);
      {
        auto seed = f.volume.open("/共存.txt", O_CREAT | O_RDWR);
        assert(seed && seed.write("utf8", 4) == 4 && seed.sync());
        seed.close();
        f.volume.end();
        ::FsVolume ordinary;
        assert(ordinary.begin(&f.disk, false, 1));
        ::FsFile ascii = ordinary.open("/stock.txt", O_CREAT | O_RDWR);
        assert(ascii && ascii.write("stock", 5) == 5 && ascii.sync());
        ascii.close();
        ordinary.end();
        assert(f.volume.begin(&f.disk, false, 1));
        char data[5];
        auto original = f.volume.open("/共存.txt", O_RDONLY);
        assert(original && original.read(data, 4) == 4 && !memcmp(data, "utf8", 4));
        original.close();
        auto shared = f.volume.open("/stock.txt", O_RDONLY);
        assert(shared && shared.read(data, 5) == 5 && !memcmp(data, "stock", 5));
        shared.close();
        assert(f.volume.remove("/共存.txt") && f.volume.remove("/stock.txt"));
      }
      if (!exfat) {
        std::vector<std::string> spellings;
        for (unsigned n = 1; n <= 19; ++n) {
          spellings.push_back(std::string("/") + char('A' + n) + "Mixed_" +
                              std::string(n * 13 - 7, char('a' + n)));
          auto file = f.volume.open(spellings.back().c_str(), O_CREAT | O_RDWR);
          assert(file);
          file.close();
        }
        for (auto &spelling : spellings) {
          std::string alias = "/" + f.alias(spelling.c_str());
          auto file = f.volume.open(alias.c_str(), O_RDONLY);
          char stored[PathSize];
          assert(file && file.getName(stored, sizeof(stored)));
          assert(spelling.substr(1) == stored);
          file.close();
          assert(f.volume.remove(alias.c_str()));
        }
      }
      auto &fs = f.fs;
      auto root = fs.root();
      // Files supplied by another filesystem client acquire one identity even
      // when their first NFS lookup uses an alias instead of the stored spelling.
      fat::FsBaseFile preexisting = f.volume.open("/Preexisting_中文_É.txt", O_CREAT | O_RDWR);
      assert(preexisting);
      preexisting.close();
      std::string firstName = exfat ? "preexisting_中文_é.TXT" : f.alias("/Preexisting_中文_É.txt");
      auto beforeResolution = f.raw.sectorWrites;
      auto preId = lookup(fs, root, firstName.c_str());
      assert(lookup(fs, root, "Preexisting_中文_É.txt") == preId);
      path(fs, preId, "/Preexisting_中文_É.txt");
      assert(f.raw.sectorWrites == beforeResolution);
      assert(fs.remove(root, firstName.c_str(), false) == Ok);

      auto dir = create(fs, root, "中文-目录", true);
      auto target = create(fs, root, "目标-目录", true);
      auto file = create(fs, dir, "Display_中文_Écran.bmp");
      auto id = lookup(fs, dir, "display_中文_éCRAN.BMP");
      assert(file == id);
      auto metadataWrites = f.raw.sectorWrites;
      path(fs, id, "/中文-目录/Display_中文_Écran.bmp");
      uint64_t resolved;
      assert(fs.resolvePath("/中文-目录/DISPLAY_中文_écran.BMP", resolved) == Ok &&
             resolved == file);
      std::string dirAlias = "中文-目录", fileAlias = "Display_中文_Écran.bmp";
      if (!exfat) {
        dirAlias = f.alias("/中文-目录");
        fileAlias = f.alias("/中文-目录/Display_中文_Écran.bmp");
        assert(lookup(fs, root, dirAlias.c_str()) == dir);
        assert(lookup(fs, dir, fileAlias.c_str()) == file);
        auto absolute = "/" + dirAlias + "/" + fileAlias;
        assert(fs.resolvePath(absolute.c_str(), resolved) == Ok && resolved == file);
      }
      assert(f.raw.sectorWrites == metadataWrites);
      std::set<uint64_t> protectedFiles = {file};
      assert(protectedFiles.count(lookup(fs, dir, fileAlias.c_str())));
      std::vector<uint8_t> model(4096);
      for (size_t i = 0; i < model.size(); ++i)
        model[i] = uint8_t(i * 7);
      size_t n = model.size();
      assert(fs.write(file, 0, model.data(), n) == Ok && n == model.size());
      for (unsigned i = 0; i < 20; ++i) {
        size_t off = (i * 137) % 3900;
        n = 73;
        uint8_t patch[73];
        memset(patch, i, 73);
        assert(fs.write(file, off, patch, n) == Ok);
        memcpy(model.data() + off, patch, n);
      }
      uint8_t out[4096];
      n = sizeof(out);
      assert(fs.read(id, 0, out, n) == Ok && n == model.size() && !memcmp(out, model.data(), n));
      // Enumerated identity and LOOKUP aliases converge, while lookup leaves cookies unchanged.
      uint64_t ver, after;
      assert(fs.directoryVerifier(dir, ver) == Ok);
      Entry entry;
      bool eof;
      assert(fs.readdir(dir, 0, 0, entry, eof) == Ok && !eof && entry.id == file);
      assert(!strcmp(entry.name, "Display_中文_Écran.bmp"));
      assert(fs.readdirNames(dir, 0, 0, entry, eof) == Ok && entry.id == file);
      assert(fs.directoryVerifier(dir, after) == Ok && after == ver);
      auto extra = create(fs, dir, "图片😀.png");
      assert(fs.readdir(dir, 1, ver, entry, eof) == BadCookie);
      assert(fs.remove(dir, "图片😀.png", false) == Ok);
      Attr attr;
      assert(fs.getattr(extra, attr) == Stale);
      // NFC and NFD stay independently addressable, including their native case variants.
      auto nfc = create(fs, root, "é-文件.txt"), nfd = create(fs, root, "é-文件.txt");
      assert(nfc != nfd);
      assert(lookup(fs, root, "É-文件.TXT") == nfc);
      assert(lookup(fs, root, "É-文件.TXT") == nfd);
      path(fs, nfc, "/é-文件.txt");
      path(fs, nfd, "/é-文件.txt");
      assert(fs.remove(root, "É-文件.TXT", false) == Ok &&
             fs.remove(root, "É-文件.TXT", false) == Ok);
      auto sigma = create(fs, root, "Σ-中文");
      assert(lookup(fs, root, "ς-中文") == sigma);
      assert(fs.remove(root, "σ-中文", false) == Ok);
      auto german = create(fs, root, "Straße"), expanded = create(fs, root, "STRASSE");
      assert(german != expanded);
      assert(fs.remove(root, "Straße", false) == Ok && fs.remove(root, "STRASSE", false) == Ok);
      // Invalid encodings, separators and SdFat-trimmed spellings cause no media writes.
      for (const char *bad :
           {"\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f", "\x80",
            "a/b", "a\\b", "a:", "name.", "name ", " name", "a\x01"}) {
        auto writes = f.raw.sectorWrites;
        uint64_t rejected = 0;
        assert(fs.create(root, bad, false, 0, 0, SetAttr(), rejected) == Invalid);
        assert(f.raw.sectorWrites == writes);
        assert(fs.lookup(root, bad, rejected) == Invalid);
      }
      char tiny[2];
      assert(fs.canonicalPath(file, tiny, sizeof(tiny)) == NameTooLong);
      assert(fs.resolvePath("/../escape", resolved) == Invalid &&
             fs.resolvePath("//double", resolved) == Invalid);
      // Source alias length never determines subtree rewrite offsets.
      assert(fs.rename(dir, fileAlias.c_str(), target, "Moved_中文_Écran.bmp") == Ok);
      path(fs, file, "/目标-目录/Moved_中文_Écran.bmp");
      assert(lookup(fs, target, "moved_中文_écran.BMP") == file);
      std::string targetAlias = exfat ? "目标-目录" : f.alias("/目标-目录");
      assert(fs.rename(root, targetAlias.c_str(), root, "迁移-目录") == Ok);
      path(fs, file, "/迁移-目录/Moved_中文_Écran.bmp");
      n = sizeof(out);
      assert(fs.read(file, 0, out, n) == Ok && !memcmp(out, model.data(), n));
      if (transactional) {
        assert(fs.rename(target, "Moved_中文_Écran.bmp", target, "MOVED_中文_écran.BMP") == Ok);
        path(fs, file, "/迁移-目录/MOVED_中文_écran.BMP");
        auto victim = create(fs, dir, "Victim_中文_文件.bmp");
        std::string dest = "Victim_中文_文件.bmp";
        if (!exfat)
          dest = f.alias("/中文-目录/Victim_中文_文件.bmp");
        assert(fs.rename(target, "MOVED_中文_écran.BMP", dir, dest.c_str()) == Ok);
        assert(fs.getattr(victim, attr) == Stale);
        assert(lookup(fs, dir, dest.c_str()) == file);
        path(fs, file, "/中文-目录/Victim_中文_文件.bmp");
        auto exclusive = create(fs, root, "独占-É.txt", false, 2, 0x123456789ULL);
        assert(create(fs, root, "独占-é.TXT", false, 2, 0x123456789ULL) == exclusive);
        if (!exfat) {
          auto alias = f.alias("/独占-É.txt");
          assert(create(fs, root, alias.c_str(), false, 2, 0x123456789ULL) == exclusive);
        }
        assert(fs.rename(root, "独占-é.TXT", root, "独占-é.txt") == Ok);
        assert(create(fs, root, "独占-É.TXT", false, 2, 0x123456789ULL) == exclusive);
        assert(fs.remove(root, "独占-é.TXT", false) == Ok);
        if (!exfat) {
          auto secret = f.alias("/.esp32-nfs");
          std::string q = "/" + secret + "/exclusive.bin";
          Attr a;
          char canonical[PathSize];
          assert(f.backend.resolve(q.c_str(), canonical, sizeof(canonical), a) == NoEnt);
          assert(f.backend.create(("/" + secret + "/attack").c_str(), false) == Access);
          assert(f.backend.remove(q.c_str(), false) == Access);
        }
      }
      // SdFat's native table keeps long-s distinct here; generic Unicode casefold would merge it.
      auto distinct = create(fs, root, ".eſp32-nfs", true);
      assert(lookup(fs, root, ".eſp32-nfs") == distinct);
      assert(fs.remove(root, ".eſp32-nfs", true) == Ok);
      assert(SdFatVerifiers::reserved("/.EsP32-NfS"));
      assert(f.backend.create("/.EsP32-NfS", true) == Access);
      char finalPath[PathSize];
      assert(fs.canonicalPath(file, finalPath, sizeof(finalPath)) == Ok);
      std::string parent = finalPath;
      auto slash = parent.rfind('/');
      std::string leaf = parent.substr(slash + 1);
      parent.resize(slash);
      uint64_t parentId;
      assert(fs.resolvePath(parent.c_str(), parentId) == Ok);
      if (!exfat)
        leaf = f.alias(finalPath);
      assert(fs.remove(parentId, leaf.c_str(), false) == Ok);
      assert(fs.getattr(file, attr) == Stale);
      uint64_t replacement = create(fs, parentId, "new-中文");
      assert(replacement != file);
      assert(fs.remove(parentId, "new-中文", false) == Ok);
      // Short aliases cannot bypass the full canonical path budget, even with a larger output
      // buffer.
      if (!exfat) {
        f.backend.invalidateCaches();
        std::string longDir(128, 'd'), longFile(128, 'f');
        auto parent = "/" + longDir, whole = parent + "/" + longFile;
        assert(f.volume.mkdir(parent.c_str(), false));
        fat::FsBaseFile large = f.volume.open(whole.c_str(), O_CREAT | O_RDWR);
        assert(large);
        large.close();
        auto shortPath = "/" + f.alias(parent.c_str()) + "/" + f.alias(whole.c_str());
        char big[1024];
        Attr a;
        auto before = f.raw.sectorWrites;
        assert(f.backend.resolve(shortPath.c_str(), big, sizeof(big), a) == NameTooLong);
        assert(f.raw.sectorWrites == before);
        assert(f.volume.remove(whole.c_str()) && f.volume.rmdir(parent.c_str()));
        f.backend.invalidateCaches();
      }
      printf("%s %s Unicode, aliases, identities, cookies, rename, invalid input: PASS\n",
             exfat ? "exFAT" : "FAT32", transactional ? "journal" : "direct");
    }
  return 0;
}

#include "fixture.h"
static_assert(USE_UTF8_LONG_NAMES == 0, "Test the installed SdFat default configuration");
#include "transports/SocketTransport.h"
#include <chrono>
#include <signal.h>
#include <thread>
volatile sig_atomic_t run = 1;
void stopped(int) {
  run = 0;
}
int main(int argc, char **argv) {
  if (argc == 1)
    return checks();
  if (argc != 4)
    return 2;
  MemoryLimits limits;
  limits.maxBytes = 192 * 1024;
  assert(defaultMemory().configure(limits));
  Fixture f(!strcmp(argv[1], "exfat"), true);
  if (!f.backend.refreshSpace())
    return 4;
  SocketTransport network(INADDR_LOOPBACK);
  esp32_nfs::Server server(f.fs);
  Config config;
  config.port = uint16_t(atoi(argv[2]));
  config.blockSize = uint32_t(atoi(argv[3]));
  config.bytesPerSecond = 0;
  config.ioBytesPerPoll = 8192;
  config.directoryEntriesPerRpc = 16;
  if (!server.begin(network, config, 1234567))
    return 3;
  signal(SIGTERM, stopped);
  signal(SIGINT, stopped);
  std::string alias = "-";
  if (!strcmp(argv[1], "fat")) {
    auto directory = create(f.fs, f.fs.root(), "别名-目录", true);
    create(f.fs, directory, "Display_中文_测试.bin");
    alias = "/" + f.alias("/别名-目录") + "/" + f.alias("/别名-目录/Display_中文_测试.bin");
  }
  assert(f.volume.mkdir("/bulk", false));
  for (unsigned i = 0; i < 1100; ++i) {
    char p[80];
    snprintf(p, sizeof(p), "/bulk/F%04u-中文.txt", i);
    auto file = f.volume.open(p, O_CREAT | O_RDWR);
    assert(file && file.write("x", 1) == 1 && file.close());
  }
  printf("READY %u %s\n", config.port, alias.c_str());
  fflush(stdout);
  while (run) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    server.poll(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  server.stop();
  return 0;
}
