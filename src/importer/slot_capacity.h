// wwe2k14 - the character-slot capacity of WWE 2K14, in one place (importer core, game side, launchers).
//
// This header is THE definition of every character-id band the Superstar Importer may use. It exists because the
// same numbers (397, 499, 103) were repeated in importer_core.h/.cpp, pack_fit.h and the UIs: a change in one place
// silently disagreed with the others, and nothing checked them against the game's own tables.
//
// WHAT THE GAME'S TABLES HOLD (measured, not assumed)
// ---------------------------------------------------
// chEtc (pac\ch\chEtc[_dlcPP].pac: CHAR/DAT, CHAR/DSBD, CHAR/PRO) holds exactly 501 leaves, one per character id,
// keyed by that id. tools\importer\build_mod_pack.py patch_rows says it for all three: "DAT: 501 leaves (id =
// character id)", "DSBD: 501 raw 80-byte leaves", "PRO: 501 'BPE ' leaves"; the C++ port that builds our pack
// (src\importer\importer_core.cpp PatchChEtc) reads and writes those same leaves.
//     0 ..  99   Create-a-Superstar slots (their rows carry rank 1000+)
//   100 .. 499   the roster (rank 1..140 in pack 5). The main save keeps a mirror of exactly these: 400 rows of
//                204 bytes, row i = character 100 + i (tools\census\roster_classes.py active_ids(): payload
//                0x1C + 204*i for i in 0..399, active byte +16, pack code +17)
//       500      one special row (rank 2000)
// There is no 502nd leaf, no roster row for id 501+, and no CAW row at id 100. Those are STRUCTURAL limits of the
// game's tables and its save file, not of this project's code.
//
// THE EXTRA-CHARACTER BAND
// ------------------------
// ids 397..499 are the only roster rows that ship blank, so they are the ids the importer hands out. They are
// genuine non-CAW characters (roster rows, models, entrances, movesets, music) - never CAW slots, and never a
// re-use of an existing character id.
//
// WHY THE BAND CANNOT BE WIDENED IN ONE PASS (dependencies, in order)
//   1. chEtc CHAR/DAT, CHAR/DSBD and CHAR/PRO have a leaf for every id up to 500 only. A character at id 501+ has
//      no roster row to clone; the build would have to append leaves the game's reader was never shown, and
//      whether its roster array then grows is unknown and untested.
//   2. The main save mirrors the 400 roster rows (ids 100..499). An id above 499 has no save row, so that
//      character's roster edits/progress have nowhere to go.
//   3. The attire id is `character id * 100 + attire` in a u16 (chEtc record +116..+199 and CHAR/DSBD), so a
//      character id above 655 cannot be encoded at all (asserted below).
//   4. Entrance banks are named `10000 + 1000 * G + character id` for G = 0..9 (importer_core.cpp FindBanks /
//      RetargetEv), so an id of 1000+ would collide with the next G's bank; character pacs and titantrons are
//      3-digit names (CHnnn.PAC / NNN.bik2), i.e. id <= 999.
// Together: 397..499 is both what the tables hold and what every related file format can name. The band is full.
//
// WIDENING IT FOR REAL means growing the game's roster/CAW arrays and the save block (its 28-byte header holds the
// block sizes and a checksum of everything after it), then rebuilding chEtc with the extra leaves: changes inside
// the recompiled game, not in the pack. Until that exists, kSlotCount is the honest capacity.
#pragma once

#include <cstdint>

namespace importer {

// The three bands of the chEtc tables (501 leaves in total).
inline constexpr int kCawFirstSlot = 0, kCawLastSlot = 99;
inline constexpr int kRosterFirstId = 100, kRosterLastId = 499;
inline constexpr int kSpecialRosterId = 500;
inline constexpr int kChetcLeaves = 501;  // one leaf per id 0..500 in CHAR/DAT, CHAR/DSBD and CHAR/PRO
inline constexpr int kCawSlots = kCawLastSlot - kCawFirstSlot + 1;       // 100
inline constexpr int kRosterSlots = kRosterLastId - kRosterFirstId + 1;  // 400 (the save's mirror)

// The blank roster rows: the importer's extra (non-CAW) character slots.
inline constexpr int kFirstSlot = 397, kLastSlot = 499;
inline constexpr int kSlotCount = kLastSlot - kFirstSlot + 1;  // 103
inline constexpr int kMaxStars = kSlotCount;                   // the pack's cap IS the band (no separate number)
inline constexpr int kMinBase = kRosterFirstId, kMaxBase = kFirstSlot - 1;

// The formats' encodings these ids must survive.
inline constexpr int kAttireIdsPerCharacter = 100;        // attire id = id * 100 + attire, u16
inline constexpr int kEntranceBankFirstVariant = 10000;   // 10000 + 1000 * G + id, G = 0..9
inline constexpr int kEntranceBankGroupSpan = 1000;
inline constexpr int kEntranceBankGroups = 10;

static_assert(kChetcLeaves == kCawSlots + kRosterSlots + 1, "chEtc: 100 CAW + 400 roster + 1 special leaf");
static_assert(kFirstSlot >= kRosterFirstId && kLastSlot <= kRosterLastId,
              "the extra-character band must stay inside the roster rows the save mirrors");
static_assert(kLastSlot < kSpecialRosterId, "the special row 500 is not a character slot");
static_assert(kLastSlot * kAttireIdsPerCharacter + (kAttireIdsPerCharacter - 1) <= 0xFFFF,
              "a character id whose attire ids do not fit the record's u16 cannot be written");
static_assert(kEntranceBankFirstVariant + (kEntranceBankGroups - 1) * kEntranceBankGroupSpan + kLastSlot < 20000,
              "entrance bank ids must not reach the 20000 band");

}  // namespace importer
