#!/usr/bin/env python3
"""wwe2k14 - the SaveData.Dat container, read and checked.

WHAT THE FILE IS (measured from a real save, 2026-11-16)
--------------------------------------------------------
SaveData.Dat is not a flat image: it is a zero-padded buffer whose tail holds one or more "ZLIB" blocks.

    offset 0 .. K            zeros (K was 0x2CA000 in the save this was written from; the prefix is unused space)
    each block at K:
        +0   "ZLIB"
        +4   a 32-bit field whose meaning is not established (0x3012 in that save)
        +8   compressed size (bytes that follow the 16-byte header)
        +12  uncompressed size
        +16  a zlib stream (0x78 0xDA in that save: maximum compression)
    the blocks' uncompressed bytes concatenated are the PAYLOAD (1,790,648 bytes there).

The payload starts with a 28-byte header of seven big-endian 32-bit words:

    +0  block 1 size (1,189,036)      +4  block 2 size (601,536)
    +8  48      +12 55      +16 42    +20 2
    +24 a value that behaves like a checksum of everything after the header (33,341,499)

Then the roster mirror the game keeps, at 0x1C, exactly as src/importer/slot_capacity.h documents it:

    400 rows of 204 bytes, row i = character id 100 + i
    +16  active flag        +17  pack code

`report` prints all of that for a save, and `text` lists the printable text runs (team, arena and superstar names).

The CAS data of a save with one created superstar (“THE SUPERSTAR”) sits in the payload at 0x1E517.. (~0x20100),
a region that is pure zeros in a save with no created superstar, and the name appears there three times
(0x1E5E8, 0x1E654, 0x1F1C0): name, and two display copies. The record layout inside that region is NOT mapped to
slots yet (one CAW is not enough to see a stride), so `names` reports text, not slots. See "What is still unknown"
below.

WHAT IS STILL UNKNOWN (blockers, not guesses)
---------------------------------------------
1. The CAS slot stride: which bytes of 0x1E517.. belong to slot 0, and where slots 1..99 begin. Two or three created
   superstars (a second and third save) settle it; then the table can be sliced per slot and the CAW library can
   swap records in and out.
2. The payload header's +24 word. It is not CRC-32, Adler-32 or a 32-bit sum of the blocks (all checked against two
   different saves of the same size: 0x1FCC03B and 0x1FF8478), so `write` is deliberately not implemented: a
   rewritten save could be rejected as corrupt, and a corrupt save loses the player's data. More before/after save
   pairs should solve it.

USAGE
-----
    python tools/save/savedata.py report   <SaveData.Dat>
    python tools/save/savedata.py dump     <SaveData.Dat> <payload.bin>
    python tools/save/savedata.py rows     <SaveData.Dat> [--active]
    python tools/save/savedata.py text     <SaveData.Dat>      printable text runs (tag teams, arenas, names)

Exit status: 0 ok, 2 a bad or unsupported save (the message says which check failed).
"""

from __future__ import annotations

import struct
import sys
import zlib

MAGIC = b"ZLIB"
BLOCK_HEADER = 16
PAYLOAD_HEADER = 28
ROSTER_BASE = 0x1C
ROSTER_ROWS = 400
ROSTER_STRIDE = 204
ROSTER_ACTIVE = 16
ROSTER_PACK = 17


class SaveError(Exception):
    """A save that cannot be read: the message names the failed check."""


class Block:
    __slots__ = ("offset", "field", "compressed", "uncompressed", "payload_off")

    def __init__(self, offset: int, field: int, compressed: int, uncompressed: int, payload_off: int) -> None:
        self.offset = offset
        self.field = field
        self.compressed = compressed
        self.uncompressed = uncompressed
        self.payload_off = payload_off


class Save:
    __slots__ = ("path", "size", "blocks", "payload", "prefix")

    def __init__(self, path: str, size: int, prefix: int, blocks: list[Block], payload: bytes) -> None:
        self.path = path
        self.size = size
        self.prefix = prefix
        self.blocks = blocks
        self.payload = payload

    # -- the payload's own header ------------------------------------------------------------------------------
    def header(self) -> list[int]:
        if len(self.payload) < PAYLOAD_HEADER:
            raise SaveError(f"payload is only {len(self.payload)} bytes: no {PAYLOAD_HEADER}-byte header")
        return list(struct.unpack(">7I", self.payload[:PAYLOAD_HEADER]))

    def roster_row(self, index: int) -> bytes:
        off = ROSTER_BASE + ROSTER_STRIDE * index
        if off + ROSTER_STRIDE > len(self.payload):
            raise SaveError(f"roster row {index} runs past the payload")
        return self.payload[off:off + ROSTER_STRIDE]


def read(path: str) -> Save:
    with open(path, "rb") as f:
        data = f.read()
    start = data.find(MAGIC)
    if start < 0:
        raise SaveError("no ZLIB block found (not a WWE 2K14 SaveData.Dat?)")
    blocks: list[Block] = []
    payload = bytearray()
    off = start
    while off + BLOCK_HEADER <= len(data) and data[off:off + 4] == MAGIC:
        field, csize, usize = struct.unpack("<III", data[off + 4:off + BLOCK_HEADER])
        body = off + BLOCK_HEADER
        if body + csize > len(data):
            raise SaveError(f"block at 0x{off:X} says {csize} compressed bytes but only "
                            f"{len(data) - body} are left")
        if usize > (64 << 20):
            raise SaveError(f"block at 0x{off:X} says {usize} uncompressed bytes (over 64 MiB): refusing")
        raw = zlib.decompress(data[body:body + csize])
        if len(raw) != usize:
            raise SaveError(f"block at 0x{off:X} inflated to {len(raw)} bytes, header says {usize}")
        blocks.append(Block(off, field, csize, usize, len(payload)))
        payload += raw
        off = body + csize
    return Save(path, len(data), start, blocks, bytes(payload))


def text_runs(payload: bytes, min_len: int = 5):
    """(offset, text) for every run of printable ASCII at least min_len long, ended by a NUL or a non-printable byte.
    Version numbers and part ids are printable too, so these are candidates, not a parsed field."""
    out = []
    start = None
    for i, c in enumerate(payload):
        printable = 0x20 <= c <= 0x7E
        if printable and start is None:
            start = i
        elif not printable and start is not None:
            if i - start >= min_len:
                out.append((start, payload[start:i].decode("ascii")))
            start = None
    if start is not None and len(payload) - start >= min_len:
        out.append((start, payload[start:].decode("ascii")))
    return out


CAS_START = 0x1E517   # measured: all zeros in a save with no created superstar, the CAW records in one that has them
CAS_END = 0x20100
NAME_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ &.-'")
VOWELS = set("AEIOUY")


def name_candidates(payload: bytes, min_len: int = 6):
    """(offset, text) for runs that look like a name the save stores: capitals, held between NUL bytes (a created
    superstar's name: "THE SUPERSTAR"). Binary tables that happen to be printable are rejected by the character set
    (no digits), the length, the NUL framing and the requirement of two different vowels ("PZPPPKKFPUKUZP" has one)."""
    out = []
    for off, text in text_runs(payload, min_len):
        if len(text) < min_len or not set(text) <= NAME_CHARS:
            continue
        letters = sum(1 for ch in text if ch != " ")
        if len(set(text) & VOWELS) < 2 or sum(1 for ch in text if ch in VOWELS) * 5 < letters:
            continue
        if off > 0 and payload[off - 1] != 0:
            continue
        end = off + len(text)
        if end < len(payload) and payload[end] != 0:
            continue
        out.append((off, text))
    return out


def check(path: str) -> Save:
    """read() plus the structural checks that do not need the game."""
    save = read(path)
    hdr = save.header()
    b1, b2 = hdr[0], hdr[1]
    if b1 + b2 > len(save.payload):
        raise SaveError(f"header blocks ({b1} + {b2}) are larger than the {len(save.payload)}-byte payload")
    if ROSTER_BASE + ROSTER_ROWS * ROSTER_STRIDE > len(save.payload):
        raise SaveError("payload is too small for the 400-row roster mirror")
    return save


def report(path: str) -> str:
    save = check(path)
    hdr = save.header()
    active = [i for i in range(ROSTER_ROWS) if save.roster_row(i)[ROSTER_ACTIVE] == 1]
    packs = sorted({save.roster_row(i)[ROSTER_PACK] for i in range(ROSTER_ROWS)})
    distinct = len({save.roster_row(i) for i in range(ROSTER_ROWS)})
    out = [
        f"save        {path}",
        f"file        {save.size} bytes, {save.prefix} zero bytes before the first ZLIB block",
        f"blocks      {len(save.blocks)}",
    ]
    for b in save.blocks:
        out.append(f"  @0x{b.offset:X}  compressed {b.compressed}  uncompressed {b.uncompressed}  "
                   f"field4 0x{b.field:X}  payload at 0x{b.payload_off:X}")
    names = name_candidates(save.payload)
    caw_names = [t for off, t in names if CAS_START <= off < CAS_END]
    out += [
        f"payload     {len(save.payload)} bytes",
        f"header      block1 {hdr[0]}  block2 {hdr[1]}  words {hdr[2:6]}  checksum? 0x{hdr[6]:X}",
        f"roster      0x{ROSTER_BASE:X} + {ROSTER_ROWS} x {ROSTER_STRIDE}: {len(active)} active, "
        f"packs {packs}, {distinct} distinct rows",
        f"blocks fit  {'yes' if hdr[0] + hdr[1] <= len(save.payload) else 'NO'}",
        f"text        {len(names)} name-like run(s); `text` lists them all with offsets",
        f"CAS region  0x{CAS_START:X}..0x{CAS_END:X}: "
        + (", ".join(caw_names) if caw_names else "no text (no created superstars)"),
        "",
        "CAS data    created-superstar records live at payload 0x1E517.. (~0x20100); the per-slot stride is not mapped",
        "            yet (needs two or three created superstars to measure) - see this file's header comment.",
        "writing     not implemented: the payload header's +24 word is not CRC-32/Adler-32/a 32-bit sum of either",
        "            sample, so a rewritten save could be refused as corrupt.",
    ]
    return "\n".join(out)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__.strip().split("USAGE")[-1].replace("-----", "").strip(), file=sys.stderr)
        return 2
    cmd, path = argv[1], argv[2]
    try:
        if cmd == "report":
            print(report(path))
        elif cmd == "dump":
            if len(argv) < 4:
                print("dump needs an output path", file=sys.stderr)
                return 2
            save = check(path)
            with open(argv[3], "wb") as f:
                f.write(save.payload)
            print(f"{len(save.payload)} payload bytes -> {argv[3]}")
        elif cmd == "text":
            save = check(path)
            for off, text in name_candidates(save.payload):
                print(f"  0x{off:06X}  {text}")
        elif cmd == "rows":
            save = check(path)
            only_active = "--active" in argv
            for i in range(ROSTER_ROWS):
                row = save.roster_row(i)
                on = row[ROSTER_ACTIVE] == 1
                if only_active and not on:
                    continue
                print(f"  row {i:3d}  id {row[0] << 8 | row[1]:3d}  active {int(on)}  pack {row[ROSTER_PACK]:3d}")
        else:
            print(f"unknown command {cmd!r}", file=sys.stderr)
            return 2
    except SaveError as e:
        print(f"save error: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
