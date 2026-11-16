// wwe2k14 - self-test for the CAW library (src\caw_library\caw_library.h).
//
// Runs without the game, without the SDK and without the save: every case is files in a temporary folder. Built by
// tools\caw_library\run_tests.bat (MSVC) and by the CMake test target when there is one.
//
//   caw_library_test.exe            every case, including the 5000-CAW capacity run
//   caw_library_test.exe --quick    the same cases without the 5000-CAW run (a few seconds faster)

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "caw_library.h"

namespace {

int g_checks = 0;
int g_failed = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failed;                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    ++g_checks;                                                            \
    const auto va = (a);                                                   \
    const auto vb = (b);                                                   \
    if (!(va == vb)) {                                                     \
      ++g_failed;                                                          \
      std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);   \
    }                                                                      \
  } while (0)

using namespace wwe2k14::cawlib;
namespace fs = std::filesystem;

fs::path TempRoot(const char* name) {
  std::error_code ec;
  const fs::path p = fs::temp_directory_path(ec) / (std::string("wwe2k14-cawlib-") + name);
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

fs::path WriteFile(const fs::path& dir, const std::string& name, const std::string& bytes) {
  fs::path p = dir / name;
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), std::streamsize(bytes.size()));
  return p;
}

std::string ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void TestBasics() {
  const fs::path root = TempRoot("basics");
  const fs::path src = root / "in";
  fs::create_directories(src);

  Library lib;
  CHECK(lib.Open(root / "lib") == Status::kOk);
  CHECK_EQ(lib.Count(), 0);
  CHECK(lib.limits().max_caws == kMaxCaws);
  CHECK(lib.limits().max_slots == kMaxLiveSlots);

  const fs::path a = WriteFile(src, "a.cas", "AAAA-payload");
  const fs::path b = WriteFile(src, "b.cas", "BBBB-payload");
  const Result ra = lib.Add(a, "Alpha");
  CHECK(ra.ok());
  CHECK(!ra.existing);
  CHECK(!ra.id.empty());
  CHECK_EQ(lib.Count(), 1);
  CHECK(fs::is_regular_file(root / "lib" / kPayloadDirName / (ra.id + ".cas")));

  // The same bytes again: one entry, and the caller learns it was already there.
  const Result dup = lib.Add(a, "Alpha copy");
  CHECK(dup.ok());
  CHECK(dup.existing);
  CHECK_EQ(dup.id, ra.id);
  CHECK_EQ(lib.Count(), 1);

  const Result rb = lib.Add(b, "Beta");
  CHECK(rb.ok());
  CHECK_EQ(lib.Count(), 2);
  CHECK(!lib.Full());
  CHECK_EQ(lib.Free(), kMaxCaws - 2);
  CHECK_EQ(lib.UsedBytes(), uint64_t(std::strlen("AAAA-payload") + std::strlen("BBBB-payload")));

  // Names.
  CHECK_EQ(DisplayName(*lib.Find(ra.id)), std::string("Alpha"));
  CHECK(lib.Rename(ra.id, "  Alpha Prime  ").ok());
  CHECK_EQ(lib.Find(ra.id)->name, std::string("Alpha Prime"));
  CHECK(lib.Rename(ra.id, "   ") .status == Status::kBadInput);
  CHECK(lib.Rename("nope", "x").status == Status::kNotFound);

  // Mounting: the game's slot 5 gets Alpha.
  CHECK(lib.Mount(5, ra.id).ok());
  CHECK_EQ(lib.SlotId(5), ra.id);
  CHECK_EQ(lib.SlotId(4), std::string());
  CHECK_EQ(lib.Mounted().size(), size_t(1));
  CHECK(lib.Mount(kMaxLiveSlots, ra.id).status == Status::kBadInput);
  CHECK(lib.Mount(-1, ra.id).status == Status::kBadInput);
  CHECK(lib.Mount(6, "nope").status == Status::kNotFound);

  // The plan between the current mapping and a wanted one.
  std::vector<std::string> desired(size_t(6), std::string());
  desired[0] = ra.id;
  desired[5] = rb.id;
  desired[4] = "not-in-the-library";
  std::vector<std::string> errors;
  const std::vector<PlanOp> ops = lib.Plan(desired, &errors);
  CHECK_EQ(ops.size(), size_t(2));
  CHECK_EQ(ops[0].slot, 0);
  CHECK_EQ(ops[0].to, ra.id);
  CHECK_EQ(ops[1].slot, 5);
  CHECK_EQ(ops[1].from, ra.id);
  CHECK_EQ(ops[1].to, rb.id);
  CHECK_EQ(errors.size(), size_t(1));

  // Staging a live slot out to a file.
  CHECK(lib.Stage(5, root / "staged" / "slot5.cas") == Status::kOk);
  CHECK_EQ(ReadFile(root / "staged" / "slot5.cas"), std::string("AAAA-payload"));
  CHECK(lib.Stage(4, root / "staged" / "slot4.cas") == Status::kNotFound);

  // Persistence: everything above comes back from library.tsv.
  Library again;
  CHECK(again.Open(root / "lib") == Status::kOk);
  CHECK_EQ(again.Count(), 2);
  CHECK_EQ(again.SlotId(5), ra.id);
  CHECK_EQ(again.Find(ra.id)->name, std::string("Alpha Prime"));
  CHECK(fs::is_regular_file(root / "lib" / kManifestBackupName));

  // Removing a mounted CAW clears its slot instead of leaving a dangling id.
  CHECK(again.Remove(ra.id).ok());
  CHECK_EQ(again.Count(), 1);
  CHECK_EQ(again.SlotId(5), std::string());
  CHECK(again.Remove(ra.id).status == Status::kNotFound);

  // A damaged line never costs the other records.
  {
    std::ofstream f(root / "lib" / kManifestName, std::ios::app);
    f << "caw\tbad-record\n";
    f << "slot\t999\twhatever\n";
  }
  Library third;
  CHECK(third.Open(root / "lib") == Status::kOk);
  CHECK_EQ(third.Count(), 1);
  CHECK(third.warnings().size() >= 2);
}

void TestCapacity() {
  const fs::path root = TempRoot("capacity");
  const fs::path src = root / "in";
  fs::create_directories(src);
  Limits small;
  small.max_caws = 8;
  small.max_slots = 4;
  Library lib(small);
  CHECK(lib.Open(root / "lib") == Status::kOk);
  CHECK_EQ(lib.limits().max_caws, 8);
  CHECK_EQ(lib.limits().max_slots, 4);
  for (int i = 0; i < 8; ++i) {
    const fs::path p = WriteFile(src, "c" + std::to_string(i) + ".cas", "payload-" + std::to_string(i));
    CHECK(lib.Add(p, "C" + std::to_string(i)).ok());
  }
  CHECK_EQ(lib.Count(), 8);
  CHECK(lib.Full());
  const fs::path ninth = WriteFile(src, "c8.cas", "payload-8");
  const Result over = lib.Add(ninth, "C8");
  CHECK(over.status == Status::kFull);
  CHECK_EQ(lib.Count(), 8);
  // The per-CAW cap.
  Limits tight;
  tight.max_payload_bytes = 1024;
  Library small2(tight);
  CHECK(small2.Open(root / "lib2") == Status::kOk);
  const fs::path big = WriteFile(src, "big.cas", std::string(2048, 'x'));
  CHECK(small2.Add(big, "Big").status == Status::kTooBig);
  CHECK_EQ(small2.Count(), 0);
  // A slot number over the configured live slots is refused, not clamped.
  CHECK_EQ(lib.limits().max_slots, 4);
  CHECK(lib.Mount(4, lib.Entries()[0].id).status == Status::kBadInput);
  CHECK(lib.Mount(3, lib.Entries()[0].id).ok());
}

// The point of the system: the full 5000 created superstars on disk, 100 live slots, no CAS table change.
void TestFullCapacity() {
  const fs::path root = TempRoot("capacity-full");
  const fs::path src = root / "in";
  fs::create_directories(src);
  Library lib;
  CHECK(lib.Open(root / "lib") == Status::kOk);
  for (int i = 0; i < kMaxCaws; ++i) {
    const fs::path p = WriteFile(src, "c" + std::to_string(i) + ".cas", "caw-" + std::to_string(i));
    const Result r = lib.Add(p, "CAW " + std::to_string(i));
    if (!r.ok()) {
      std::printf("add %d failed: %s\n", i, r.error.c_str());
      CHECK(false);
      return;
    }
  }
  CHECK_EQ(lib.Count(), kMaxCaws);
  CHECK(lib.Full());
  CHECK_EQ(lib.Free(), 0);
  const fs::path extra = WriteFile(src, "extra.cas", "one-too-many");
  CHECK(lib.Add(extra, "Extra").status == Status::kFull);

  // 100 live slots, each a library entry; the 101st slot does not exist.
  for (int slot = 0; slot < kMaxLiveSlots; ++slot)
    CHECK(lib.Mount(slot, lib.Entries()[size_t(slot)].id).ok());
  CHECK_EQ(lib.Mounted().size(), size_t(kMaxLiveSlots));
  CHECK(lib.Mount(kMaxLiveSlots, lib.Entries()[0].id).status == Status::kBadInput);

  // Everything comes back after a restart.
  Library again;
  CHECK(again.Open(root / "lib") == Status::kOk);
  CHECK_EQ(again.Count(), kMaxCaws);
  CHECK_EQ(again.Mounted().size(), size_t(kMaxLiveSlots));
  CHECK_EQ(again.SlotId(0), lib.Entries()[0].id);

  // Swapping slot 0 to the last CAW is one plan entry, not a rebuild.
  std::vector<std::string> wanted(static_cast<size_t>(kMaxLiveSlots), std::string());
  for (int i = 0; i < kMaxLiveSlots; ++i) wanted[size_t(i)] = again.SlotId(i);
  wanted[0] = again.Entries().back().id;
  const std::vector<PlanOp> ops = again.Plan(wanted);
  CHECK_EQ(ops.size(), size_t(1));
  CHECK_EQ(ops[0].slot, 0);
}

void TestIds() {
  CHECK_EQ(MakeId(0x1234ABCDu, 0x10), std::string("1234abcd-10"));
  CHECK_EQ(Crc32("", 0), 0u);
  const char* p = "123456789";
  CHECK_EQ(Crc32(p, 9), 0xCBF43926u);  // the standard CRC-32 check value
  CHECK_EQ(Crc32Append(Crc32("1234", 4), "56789", 5), 0xCBF43926u);
}

}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
  TestIds();
  TestBasics();
  TestCapacity();
  if (!quick) TestFullCapacity();
  std::printf("%d checks, %d failed%s\n", g_checks, g_failed, quick ? " (quick)" : "");
  return g_failed == 0 ? 0 : 1;
}
