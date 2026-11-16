// wwe2k14 - the CAW library implementation (see caw_library.h for the design notes).

#include "caw_library.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace wwe2k14::cawlib {
namespace {

constexpr uint32_t kCrcTableSeed = 0xEDB88320u;

// One shared CRC-32 table (zlib polynomial), built on first use.
struct CrcTable {
  std::array<uint32_t, 256> v{};
  CrcTable() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (kCrcTableSeed ^ (c >> 1)) : (c >> 1);
      v[i] = c;
    }
  }
};
const CrcTable& Table() {
  static const CrcTable t;
  return t;
}

std::string Trim(std::string s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return s.substr(b, e - b);
}

// Manifest text: one line per record, fields separated by single tabs. A name keeps everything after its field, so a
// name may contain spaces but not tabs or newlines (CleanName strips those).
std::string CleanName(std::string name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) continue;  // no tabs, no newlines, no controls
    out.push_back(c);
  }
  out = Trim(out);
  if (out.size() > kMaxNameLength) {
    // Cut on a byte boundary that is not the middle of a UTF-8 sequence.
    size_t n = kMaxNameLength;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
    out.resize(n);
  }
  return out;
}

int64_t NowUnix() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool ParseInt(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  *out = static_cast<int64_t>(v);
  return true;
}

bool ParseU64(const std::string& s, uint64_t* out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || *end != '\0') return false;
  *out = static_cast<uint64_t>(v);
  return true;
}

bool ParseU32(const std::string& s, uint32_t* out) {
  uint64_t v = 0;
  if (!ParseU64(s, &v) || v > 0xFFFFFFFFull) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

}  // namespace

uint32_t Crc32Append(uint32_t crc, const void* data, size_t bytes) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = crc ^ 0xFFFFFFFFu;
  const auto& t = Table().v;
  for (size_t i = 0; i < bytes; ++i) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t Crc32(const void* data, size_t bytes) { return Crc32Append(0, data, bytes); }

// "<8 hex digits>-<size in hex>": short enough for a file name, and two different payloads can only share it after a
// CRC collision AND the same size (Add compares the bytes before merging, so a collision cannot merge two CAWs).
std::string MakeId(uint32_t crc, uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%08x-%llx", crc, static_cast<unsigned long long>(bytes));
  return buf;
}

std::string DisplayName(const Entry& e) { return e.name.empty() ? e.id : e.name; }

Library::Library(Limits limits) {
  limits_.max_caws = std::clamp(limits.max_caws, 1, kMaxCaws);
  limits_.max_slots = std::clamp(limits.max_slots, 1, kMaxLiveSlots);
  limits_.max_payload_bytes = limits.max_payload_bytes == 0
                                  ? kMaxPayloadBytes
                                  : std::min<uint64_t>(limits.max_payload_bytes, kMaxPayloadBytes);
  slots_.assign(size_t(limits_.max_slots), std::string());
}

Status Library::Open(const fs::path& root) {
  root_ = root;
  entries_.clear();
  warnings_.clear();
  slots_.assign(size_t(limits_.max_slots), std::string());
  std::error_code ec;
  fs::create_directories(root_ / kPayloadDirName, ec);
  if (!fs::is_directory(root_ / kPayloadDirName, ec)) return Status::kIoError;
  return Load();
}

fs::path Library::PayloadPath(std::string_view id) const {
  return root_ / kPayloadDirName / (std::string(id) + ".cas");
}

const Entry* Library::Find(std::string_view id) const {
  if (id.empty()) return nullptr;
  for (const Entry& e : entries_)
    if (e.id == id) return &e;
  return nullptr;
}

const Entry* Library::FindMutable(std::string_view id) {
  return const_cast<const Library*>(this)->Find(id);
}

uint64_t Library::UsedBytes() const {
  uint64_t n = 0;
  for (const Entry& e : entries_) n += e.bytes;
  return n;
}

Result Library::Add(const fs::path& payload, std::string name, std::string origin) {
  Result r;
  std::error_code ec;
  if (payload.empty() || !fs::is_regular_file(payload, ec)) {
    r.status = Status::kBadInput;
    r.error = "the file does not exist";
    return r;
  }
  const uint64_t size = fs::file_size(payload, ec);
  if (ec) {
    r.status = Status::kIoError;
    r.error = "the file size could not be read";
    return r;
  }
  if (size > limits_.max_payload_bytes) {
    r.status = Status::kTooBig;
    r.error = "the CAW is larger than this library accepts (" + std::to_string(limits_.max_payload_bytes) + " bytes)";
    return r;
  }

  // Copy to a temp file while hashing, so the library never holds a half-written payload: the temp is renamed into
  // place only once the whole file is on disk.
  const fs::path tmp = root_ / kPayloadDirName / (std::string(".incoming-") + std::to_string(NowUnix()) + "-" +
                                                   std::to_string(reinterpret_cast<uintptr_t>(&r)) + ".part");
  std::ifstream in(payload, std::ios::binary);
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!in || !out) {
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    r.status = Status::kIoError;
    r.error = "the file could not be opened";
    return r;
  }
  uint32_t crc = 0;
  uint64_t copied = 0;
  std::array<char, 64 * 1024> buf{};
  while (in) {
    in.read(buf.data(), std::streamsize(buf.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    crc = Crc32Append(crc, buf.data(), size_t(got));
    out.write(buf.data(), got);
    copied += uint64_t(got);
  }
  out.flush();
  const bool write_ok = out.good();
  out.close();
  if (!in.eof() || !write_ok || copied != size) {
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    r.status = Status::kIoError;
    r.error = "the file could not be copied into the library";
    return r;
  }

  const std::string id = MakeId(crc, copied);
  if (const Entry* same = Find(id)) {
    // Same content address: confirm the bytes before calling it a duplicate.
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    r.status = Status::kOk;
    r.id = same->id;
    r.existing = true;
    return r;
  }

  if (Full()) {
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    r.status = Status::kFull;
    r.error = "the library is full (" + std::to_string(limits_.max_caws) + " CAWs)";
    return r;
  }

  const fs::path dest = PayloadPath(id);
  fs::rename(tmp, dest, ec);
  if (ec) {
    std::error_code cleanup;
    fs::remove(tmp, cleanup);
    r.status = Status::kIoError;
    r.error = "the CAW could not be moved into the library (" + ec.message() + ")";
    return r;
  }

  Entry e;
  e.id = id;
  e.name = CleanName(name.empty() ? payload.stem().string() : name);
  e.origin = origin.empty() ? payload.string() : origin;
  e.bytes = copied;
  e.crc32 = crc;
  e.added_unix = NowUnix();
  entries_.push_back(e);
  r.id = id;
  const Status saved = Flush();
  if (saved != Status::kOk) {
    r.status = saved;
    r.error = "the CAW was stored, but the library file could not be written";
    return r;
  }
  return r;
}

Result Library::Rename(std::string_view id, std::string name) {
  Result r;
  const std::string clean = CleanName(std::move(name));
  if (clean.empty()) {
    r.status = Status::kBadInput;
    r.error = "a CAW name cannot be empty";
    return r;
  }
  Entry* e = const_cast<Entry*>(FindMutable(id));
  if (!e) {
    r.status = Status::kNotFound;
    r.error = "no such CAW in the library";
    return r;
  }
  e->name = clean;
  r.id = e->id;
  r.status = Flush();
  return r;
}

Result Library::Remove(std::string_view id) {
  Result r;
  const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; });
  if (it == entries_.end()) {
    r.status = Status::kNotFound;
    r.error = "no such CAW in the library";
    return r;
  }
  for (std::string& s : slots_)
    if (s == id) s.clear();
  std::error_code ec;
  fs::remove(PayloadPath(id), ec);
  entries_.erase(it);
  r.id = std::string(id);
  r.status = Flush();
  return r;
}

Result Library::Mount(int slot, std::string_view id) {
  Result r;
  if (slot < 0 || slot >= limits_.max_slots) {
    r.status = Status::kBadInput;
    r.error = "slot " + std::to_string(slot) + " is outside 0.." + std::to_string(limits_.max_slots - 1);
    return r;
  }
  if (!Find(id)) {
    r.status = Status::kNotFound;
    r.error = "no such CAW in the library";
    return r;
  }
  slots_[size_t(slot)] = std::string(id);
  r.id = std::string(id);
  r.status = Flush();
  return r;
}

Result Library::Unmount(int slot) {
  Result r;
  if (slot < 0 || slot >= limits_.max_slots) {
    r.status = Status::kBadInput;
    r.error = "slot " + std::to_string(slot) + " is outside 0.." + std::to_string(limits_.max_slots - 1);
    return r;
  }
  slots_[size_t(slot)].clear();
  r.status = Flush();
  return r;
}

std::string Library::SlotId(int slot) const {
  if (slot < 0 || slot >= int(slots_.size())) return {};
  return slots_[size_t(slot)];
}

std::vector<Slot> Library::Mounted() const {
  std::vector<Slot> out;
  for (int i = 0; i < int(slots_.size()); ++i)
    if (!slots_[size_t(i)].empty()) out.push_back({i, slots_[size_t(i)]});
  return out;
}

std::vector<PlanOp> Library::Plan(const std::vector<std::string>& desired, std::vector<std::string>* errors) const {
  std::vector<PlanOp> ops;
  const int n = std::min<int>(int(desired.size()), limits_.max_slots);
  for (int i = 0; i < n; ++i) {
    const std::string& want = desired[size_t(i)];
    const std::string now = SlotId(i);
    if (want == now) continue;
    if (!want.empty() && !Find(want)) {
      if (errors) errors->push_back("slot " + std::to_string(i) + ": no CAW " + want + " in the library");
      continue;
    }
    ops.push_back({i, now, want});
  }
  return ops;
}

Status Library::Stage(int slot, const fs::path& dest) const {
  const std::string id = SlotId(slot);
  if (id.empty()) return Status::kNotFound;
  std::error_code ec;
  const fs::path src = PayloadPath(id);
  if (!fs::is_regular_file(src, ec)) return Status::kNotFound;
  fs::create_directories(dest.parent_path(), ec);
  fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
  return ec ? Status::kIoError : Status::kOk;
}

std::string Library::NowStamp() const { return std::to_string(NowUnix()); }

Status Library::Flush() {
  if (root_.empty()) return Status::kIoError;
  const fs::path manifest = root_ / kManifestName;
  const fs::path tmp = root_ / (std::string(kManifestName) + ".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return Status::kIoError;
    f << kManifestMagic << ' ' << kManifestVersion << '\n';
    f << "capacity\t" << limits_.max_caws << '\t' << limits_.max_slots << '\n';
    for (const Entry& e : entries_)
      f << "caw\t" << e.id << '\t' << e.bytes << '\t' << e.crc32 << '\t' << e.added_unix << '\t' << e.name << '\n';
    for (int i = 0; i < int(slots_.size()); ++i)
      if (!slots_[size_t(i)].empty()) f << "slot\t" << i << '\t' << slots_[size_t(i)] << '\n';
    f.flush();
    if (!f.good()) return Status::kIoError;
  }
  std::error_code ec;
  if (fs::is_regular_file(manifest, ec)) {
    fs::remove(root_ / kManifestBackupName, ec);
    fs::copy_file(manifest, root_ / kManifestBackupName, fs::copy_options::overwrite_existing, ec);
  }
  fs::rename(tmp, manifest, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return Status::kIoError;
  }
  return Status::kOk;
}

Status Library::Load() {
  const fs::path manifest = root_ / kManifestName;
  std::error_code ec;
  if (!fs::is_regular_file(manifest, ec)) return Status::kOk;  // a new library
  std::ifstream f(manifest, std::ios::binary);
  if (!f) return Status::kIoError;

  std::string line;
  bool magic_seen = false;
  int line_no = 0;
  while (std::getline(f, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (!magic_seen) {
      std::istringstream head(line);
      std::string magic;
      int version = 0;
      head >> magic >> version;
      if (magic != kManifestMagic || version < 1 || version > kManifestVersion) {
        warnings_.push_back("library.tsv: unknown format (first line)");
        return Status::kBadInput;
      }
      magic_seen = true;
      continue;
    }
    std::vector<std::string> field;
    size_t at = 0;
    while (true) {
      const size_t tab = line.find('\t', at);
      if (tab == std::string::npos) {
        field.push_back(line.substr(at));
        break;
      }
      field.push_back(line.substr(at, tab - at));
      at = tab + 1;
    }
    if (field.empty()) continue;
    if (field[0] == "capacity" && field.size() >= 3) {
      int64_t caws = 0, slots = 0;
      if (ParseInt(field[1], &caws) && ParseInt(field[2], &slots)) {
        // The manifest's capacity is honoured only up to this build's ceilings: a library written by a bigger build
        // still loads, and cannot be grown past what this binary can address.
        limits_.max_caws = std::clamp(int(std::min<int64_t>(caws, kMaxCaws)), 1, kMaxCaws);
        limits_.max_slots = std::clamp(int(std::min<int64_t>(slots, kMaxLiveSlots)), 1, kMaxLiveSlots);
        slots_.assign(size_t(limits_.max_slots), std::string());
      }
    } else if (field[0] == "caw" && field.size() >= 6) {
      Entry e;
      e.id = field[1];
      e.name = CleanName(field[5]);
      const bool ok = ParseU64(field[2], &e.bytes) && ParseU32(field[3], &e.crc32) && ParseInt(field[4], &e.added_unix) &&
                      !e.id.empty();
      if (!ok) {
        warnings_.push_back("library.tsv line " + std::to_string(line_no) + ": damaged CAW record skipped");
        continue;
      }
      if (int(entries_.size()) >= limits_.max_caws) {
        warnings_.push_back("library.tsv line " + std::to_string(line_no) + ": over capacity, skipped");
        continue;
      }
      entries_.push_back(std::move(e));
    } else if (field[0] == "slot" && field.size() >= 3) {
      int64_t slot = 0;
      if (!ParseInt(field[1], &slot) || slot < 0 || slot >= limits_.max_slots) {
        warnings_.push_back("library.tsv line " + std::to_string(line_no) + ": slot out of range, skipped");
        continue;
      }
      if (!Find(field[2])) {
        warnings_.push_back("library.tsv line " + std::to_string(line_no) + ": slot names a CAW that is not in the "
                            "library, left empty");
        continue;
      }
      slots_[size_t(slot)] = field[2];
    } else {
      warnings_.push_back("library.tsv line " + std::to_string(line_no) + ": unknown record skipped");
    }
  }
  if (!magic_seen) return Status::kBadInput;
  return Status::kOk;
}

}  // namespace wwe2k14::cawlib
