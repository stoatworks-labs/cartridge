#!/usr/bin/env python3
"""Build `testcard.gba` — a tiny GBA ROM of animated test patterns.

## Why this exists

This repository ships no cores, no BIOS images and no games, which leaves a
gap: there is nothing you can point `cartridge` at to see it work. Fixing that
with somebody's commercial ROM is exactly the thing the licensing section of the
README refuses to do, so instead the *source* of a ROM lives here and you build
it. Nobody has to obtain anything, and nothing copyrighted is redistributed.

It is also the right content for this plugin specifically. `cartridge` exists to
feed a 240p raster into [old-cathode](https://github.com/stoatworks-labs/old-cathode),
and the four patterns are the ones a composite path actually punishes: SMPTE
bars over a grey ramp, a drifting crosshatch, a hard vertical edge sweeping
across a ramp, and an inverting checkerboard at the raster's highest frequency.

## Why there is no linker

macOS ships clang and nothing else useful for bare-metal ARM — no devkitARM, no
`arm-none-eabi-gcc`, no `lld`, no `llvm-objcopy`. Rather than make the reader
install a toolchain, this leans on the one thing Apple's clang *can* do: it has
every LLVM backend built in, so it will assemble and compile for
`armv4t-none-eabi` even though it cannot link for it.

So the whole program is arranged to need no linking at all:

  * `rom.s` and the compiled `pattern.c` are concatenated into ONE assembly
    file, so every branch is internal and the assembler resolves it;
  * the entry stub falls THROUGH into `gbamain` rather than branching to it,
    because a branch to a global symbol leaves a relocation behind;
  * `-fno-jump-tables` stops clang emitting a table of absolute code addresses,
    which would need relocating to the 0x08000000 load address;
  * the patterns are arithmetic, with no lookup tables, so nothing lands in
    `.rodata` and there is only ever one section to extract.

The build then reads `.text` straight out of the ELF object and prepends a
cartridge header. `assert_no_relocations` below is what keeps that honest: if a
future edit reintroduces one, this fails loudly instead of emitting a ROM that
jumps somewhere arbitrary.

## The Nintendo logo is deliberately absent

A real GBA BIOS refuses to boot a cartridge whose header does not carry
Nintendo's logo bitmap at 0x04. That bitmap is Nintendo's artwork, so it is not
reproduced here and the field is left zeroed.

**The consequence, stated rather than hidden: this ROM will not boot on real
hardware.** It boots in emulators that skip the BIOS, which is what mGBA does by
default and all this needs to be.

    python3 tools/testrom/build.py [-o testcard.gba]
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

TARGET = "armv4t-none-eabi"
CFLAGS = [
    "-O2",
    "-S",
    "-ffreestanding",
    "-fno-builtin",
    # Each of these removes a relocation this build has no linker to resolve.
    "-fno-jump-tables",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
]

ENTRY = 0xC0  # the header is 192 bytes, so code starts here


def sections(obj: bytes) -> dict[str, dict]:
    """Section headers of a little-endian 32-bit ELF, by name."""
    sh_off = struct.unpack_from("<I", obj, 0x20)[0]
    sh_ent = struct.unpack_from("<H", obj, 0x2E)[0]
    sh_num = struct.unpack_from("<H", obj, 0x30)[0]
    sh_str = struct.unpack_from("<H", obj, 0x32)[0]

    fields = "name type flags addr off size link info align entsize".split()

    def header(i: int) -> dict:
        return dict(zip(fields, struct.unpack_from("<10I", obj, sh_off + i * sh_ent)))

    str_off = header(sh_str)["off"]

    def name(off: int) -> str:
        end = obj.index(b"\0", str_off + off)
        return obj[str_off + off : end].decode()

    return {name(header(i)["name"]): header(i) for i in range(sh_num)}


def assert_no_relocations(secs: dict[str, dict]) -> None:
    """A relocation in .text means an address this build cannot resolve.

    Emitting the ROM anyway would produce something that assembles, links
    nowhere, and branches into whatever happens to be at the unrelocated
    address — a black screen with no message, which is the failure this whole
    project's diagnostics exist to avoid.
    """
    rel = secs.get(".rel.text")
    count = rel["size"] // 8 if rel else 0
    if count:
        sys.exit(
            f"{count} relocation(s) in .text.\n"
            "Something now needs a linker. Usual causes: a branch to a global "
            "symbol (make it local or fall through to it), a jump table "
            "(-fno-jump-tables), or data that landed in .rodata (compute it "
            "instead of looking it up)."
        )


def header(code_len: int) -> bytearray:
    h = bytearray(ENTRY)

    # 0x00: ARM branch to the entry.  offset = (0xC0 - (0 + 8)) / 4 = 0x2E
    struct.pack_into("<I", h, 0x00, 0xEA000000 | ((ENTRY - 8) >> 2))

    # 0x04..0x9F: the Nintendo logo. Deliberately zeroed — see the module
    # docstring. This is why the ROM does not boot on real hardware.
    h[0x04:0xA0] = b"\x00" * 0x9C

    h[0xA0:0xAC] = b"CARTRIDGETST"  # title, 12 bytes, space padded
    h[0xAC:0xB0] = b"CTST"          # game code
    h[0xB0:0xB2] = b"SW"            # maker code
    h[0xB2] = 0x96                  # fixed value; a cart is rejected without it
    h[0xB3] = 0x00                  # main unit code
    h[0xB4] = 0x00                  # device type
    h[0xB5:0xBC] = b"\x00" * 7      # reserved
    h[0xBC] = 0x00                  # software version

    # Header complement check over 0xA0..0xBC. mGBA does not enforce it; a real
    # BIOS does, and getting it right costs three lines.
    chk = 0
    for b in h[0xA0:0xBD]:
        chk = (chk - b) & 0xFF
    h[0xBD] = (chk - 0x19) & 0xFF
    h[0xBE:0xC0] = b"\x00\x00"

    return h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", default=str(HERE / "testcard.gba"))
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        asm = tmp / "pattern.s"
        combined = tmp / "combined.s"
        obj = tmp / "combined.o"

        subprocess.run(
            ["clang", f"--target={TARGET}", *CFLAGS, str(HERE / "pattern.c"), "-o", str(asm)],
            check=True,
        )

        # gbamain must be local, or `b gbamain` becomes a relocation. It is only
        # global because it is the C file's one external function.
        text = asm.read_text().replace("\t.globl\tgbamain", "\t.local\tgbamain")
        asm.write_text(text)

        # One translation unit: the entry stub, then the compiled patterns. The
        # stub falls through into gbamain, so the order here is load-bearing.
        combined.write_text((HERE / "rom.s").read_text() + text)

        subprocess.run(
            ["clang", f"--target={TARGET}", "-c", str(combined), "-o", str(obj)],
            check=True,
        )

        blob = obj.read_bytes()
        secs = sections(blob)
        assert_no_relocations(secs)

        t = secs[".text"]
        code = blob[t["off"] : t["off"] + t["size"]]

    rom = bytes(header(len(code))) + code

    # Pad to a whole number of KB. mGBA does not care, but a ragged length reads
    # as a truncated download.
    if len(rom) % 1024:
        rom += b"\x00" * (1024 - len(rom) % 1024)

    Path(args.out).write_bytes(rom)
    print(f"{args.out}  {len(rom)} bytes  (header {ENTRY} + code {len(code)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
