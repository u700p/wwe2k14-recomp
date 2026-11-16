// wwe2k14 - the CAW library: thousands of created superstars kept beside the save, 100 live CAS slots.
//
// WHY THIS EXISTS
// ---------------
// The game's own Create-a-Superstar storage cannot be widened from here. Measured facts (src\importer\
// slot_capacity.h): chEtc holds one leaf per character id 0..500, ids 0..99 are the CAS rows, the main save mirrors
// 400 roster rows of 204 bytes and its 28-byte header carries the block sizes and a checksum. A 101st CAS has no
// leaf, no save row and no place in the save's block table, so anything that pretends to add one is a fake unlock.
//
// So the extra CAWs live OUTSIDE the game, in a library this project owns, and the game keeps its own 100 live slots.
// The library holds up to kMaxCaws (5000) created superstars, and up to kMaxLiveSlots (100) of them are "mounted" -
// mapped onto the game's CAS slot numbers. Mounting is a mapping, staged by Stage(): the payload for a live slot is
// copied out where the save-side writer can pick it up. When the game's CAS payloads are read/written is the one part
// that needs the save format; everything here is payload-agnostic on purpose, so that step cannot invalidate it.
//
// WHAT IS IN IT
// - Capacity with real caps: kMaxCaws (hard ceiling 5000), kMaxLiveSlots (hard ceiling 100), a per-CAW byte cap.
//   Limits are configurable below those ceilings (tests, a smaller install) and the manifest records the capacity it
//   was written with.
// - Content-addressed payloads: a CAW is stored once, named by CRC-32 + size, so importing the same file twice does
//   not spend a slot. Duplicates are verified byte-for-byte before they are merged.
// - A text manifest (library.tsv) written atomically (temp file + rename, previous copy kept as .bak) and parsed
//   leniently: one damaged line never loses the other 3999.
// - Slot mounting: Mount / Unmount / Mounted / Plan (the slot changes between the current mapping and a wanted one),
//   plus Stage, which copies a mounted payload to a destination path.
// - UsedBytes / Count / Full, for a UI that has to say "3,812 of 4,000".
//
// Portability: standard C++20 only (no SDK, no Windows headers), because the game, both launchers and the arena
// viewer all need the same rules. Files are read and written through std::filesystem.
//
// NOT DONE HERE (and why): reading a CAW out of the game's save, and writing it back, needs the CAS payload layout
// (its own save block), which is not in this tree. Stage() is the seam: once that layout is known, the writer reads
// the staged file and swaps the save block, and nothing above changes.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wwe2k14::cawlib {

namespace fs = std::filesystem;

// Hard ceilings. The library capacity is the point of the system; the live-slot ceiling is the game's own CAS table
// (ids 0..99, slot_capacity.h), so it is not a tool setting above this.
inline constexpr int kMaxCaws = 5000;
inline constexpr int kMaxLiveSlots = 100;
inline constexpr uint64_t kMaxPayloadBytes = 64ull * 1024 * 1024;  // one CAS payload
inline constexpr size_t kMaxNameLength = 48;
inline constexpr const char* kManifestName = "library.tsv";
inline constexpr const char* kManifestBackupName = "library.tsv.bak";
inline constexpr const char* kPayloadDirName = "caws";
inline constexpr const char* kManifestMagic = "wwe2k14-caw-library";
inline constexpr int kManifestVersion = 1;

// What one Library may use. Values are clamped to the ceilings above by the constructor.
struct Limits {
  int max_caws = kMaxCaws;
  int max_slots = kMaxLiveSlots;
  uint64_t max_payload_bytes = kMaxPayloadBytes;
};

// One created superstar in the library. `id` is the content address ("<crc32>-<size>"); `name` is the player's
// label; `origin` is the file it was imported from (for a UI that shows provenance).
struct Entry {
  std::string id;
  std::string name;
  std::string origin;
  uint64_t bytes = 0;
  uint32_t crc32 = 0;
  int64_t added_unix = 0;
};

// A live CAS slot and the library entry mounted in it ("" = the slot is not mounted).
struct Slot {
  int index = 0;
  std::string id;
};

enum class Status {
  kOk,
  kFull,       // the library is at its capacity
  kTooBig,     // the payload is over the per-CAW cap
  kNotFound,   // no such entry / slot is empty
  kBadInput,   // empty name, out-of-range slot, unreadable payload...
  kIoError,    // the filesystem refused a read or write
};

struct Result {
  Status status = Status::kOk;
  std::string id;        // the entry the call resolved to (kOk)
  bool existing = false; // Add: the payload was already in the library (the entry was reused)
  std::string error;     // a sentence for the player (kOk leaves it empty)
  bool ok() const { return status == Status::kOk; }
};

// One slot change: `from` ("" = empty) becomes `to` ("" = emptied).
struct PlanOp {
  int slot = 0;
  std::string from;
  std::string to;
};

uint32_t Crc32(const void* data, size_t bytes);
uint32_t Crc32Append(uint32_t crc, const void* data, size_t bytes);
std::string MakeId(uint32_t crc, uint64_t bytes);

class Library {
 public:
  explicit Library(Limits limits = {});

  // Creates <root>, <root>/caws and loads <root>/library.tsv if it is there. Never throws.
  Status Open(const fs::path& root);

  const fs::path& root() const { return root_; }
  const Limits& limits() const { return limits_; }
  int Count() const { return int(entries_.size()); }
  bool Full() const { return Count() >= limits_.max_caws; }
  uint64_t UsedBytes() const;
  const std::vector<Entry>& Entries() const { return entries_; }
  const Entry* Find(std::string_view id) const;
  int Free() const { return limits_.max_caws - Count(); }

  // Copies `payload` into the library under `name` (empty = the file's own name). A payload already present
  // (same CRC-32, size and bytes) is not stored twice: the existing entry comes back with existing = true.
  Result Add(const fs::path& payload, std::string name = {}, std::string origin = {});
  Result Rename(std::string_view id, std::string name);
  Result Remove(std::string_view id);  // also unmounts every slot using it

  // The game's CAS slots. index in [0, limits().max_slots).
  Result Mount(int slot, std::string_view id);
  Result Unmount(int slot);
  std::string SlotId(int slot) const;  // "" when empty or out of range
  std::vector<Slot> Mounted() const;

  // The slot changes between the current mapping and `desired` (desired[i] = the entry wanted in slot i; "" empties
  // it). Unknown ids are reported in `errors` (when given) and skipped; nothing is changed here.
  std::vector<PlanOp> Plan(const std::vector<std::string>& desired, std::vector<std::string>* errors = nullptr) const;

  // Writes the payload mounted in `slot` to `dest` (a file path). kNotFound when the slot is empty.
  Status Stage(int slot, const fs::path& dest) const;

  // Writes the manifest (atomically: a temp file renamed over the old one, the old one kept as .bak).
  Status Flush();

  // Lines the loader had to skip in library.tsv (a damaged line never fails the whole library).
  const std::vector<std::string>& warnings() const { return warnings_; }

 private:
  const Entry* FindMutable(std::string_view id);
  fs::path PayloadPath(std::string_view id) const;
  Status Load();
  std::string NowStamp() const;

  Limits limits_;
  fs::path root_;
  std::vector<Entry> entries_;
  std::vector<std::string> slots_;  // index = CAS slot
  std::vector<std::string> warnings_;
};

// The name a UI shows for an entry, never empty.
std::string DisplayName(const Entry& e);

}  // namespace wwe2k14::cawlib
