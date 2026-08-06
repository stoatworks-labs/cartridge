# testrom — a GBA test card you build yourself

`cartridge` ships no cores, no BIOS images and no games. That is deliberate and
it is not changing — but it leaves a real gap: there is nothing you can point the
plugin at to check your setup works. Filling that with somebody's commercial ROM
is the exact thing [the README's licensing section](../../README.md#licensing)
refuses to do.

So the **source** of a ROM lives here and you build it. Nothing copyrighted is
redistributed, and you need nothing you do not already have.

```bash
python3 tools/testrom/build.py
./build/cartest --core ~/Documents/Cartridge/cores/mgba_libretro.dylib \
                --content tools/testrom/testcard.gba --frames 30 --out /tmp/tc.png
```

The result is 1 KB. It needs **mGBA** (or any GBA core) and no BIOS.

## What it draws

Four patterns, each held for 32 drawn frames, chosen because they are what a
composite signal path actually punishes — this plugin exists to feed
[old-cathode](https://github.com/stoatworks-labs/old-cathode) a 240p raster:

| Pattern | What it is for |
|---|---|
| Eight SMPTE-order bars, scrolling, over a grey ramp strip | Colour, and a luma ramp under it |
| A drifting crosshatch | Geometry and convergence |
| A hard vertical edge sweeping across a grey ramp | The pair a composite path smears differently |
| An inverting checkerboard | The highest frequency the raster has |

## Two things that are not obvious

**It will not boot on real hardware.** A real GBA BIOS refuses any cartridge
whose header does not carry Nintendo's logo bitmap at 0x04. That bitmap is
Nintendo's artwork, so it is not reproduced here and the field is zeroed. The ROM
boots in emulators that skip the BIOS, which is what mGBA does by default.

**It draws at about a quarter of frame rate**, so the animation steps rather than
glides. The first version was twelve times slower still: a colour computed per
pixel straight into VRAM is 38400 volatile halfword stores a frame, each fighting
the PPU for the bus during active display. Every pattern here is row-coherent, so
it now builds at most two rows in IWRAM and blits them 32 bits at a time — 480
per-pixel evaluations a frame instead of 38400. Getting the rest would mean mode
4 and palette animation, which is a different program.

## Why there is no linker, and why that constrains the source

macOS ships clang and nothing else useful for bare-metal ARM — no devkitARM, no
`arm-none-eabi-gcc`, no `lld`, no `llvm-objcopy`. Rather than ask you to install
a toolchain, this leans on the one thing Apple's clang can do: every LLVM backend
is built in, so it will **compile and assemble** for `armv4t-none-eabi` even
though it cannot **link** for it.

`build.py` therefore reads `.text` straight out of the ELF object and prepends a
cartridge header. That only works while the code needs no relocations, which
shapes `pattern.c` in four ways worth knowing before you edit it:

- the entry stub in `rom.s` **falls through** into `gbamain` rather than
  branching, because a branch to a global symbol leaves a relocation;
- `gbamain` is rewritten to `.local` for the same reason;
- `-fno-jump-tables`, or clang emits a table of absolute code addresses that
  would need relocating to the 0x08000000 load address;
- the patterns are **arithmetic, never lookup tables**, so nothing lands in
  `.rodata` and there is only one section to extract.

`build.py` asserts there are no relocations left and fails loudly if a change
reintroduces one. Without that check the build would happily emit a ROM that
branches to an unrelocated address — a black screen with no message, which is the
failure mode this project's diagnostics exist to prevent.

## Making footage from it

`cartest` can write every frame, which is how the project video's test-card
material was made:

```bash
./build/cartest --core ~/Documents/Cartridge/cores/mgba_libretro.dylib \
                --content tools/testrom/testcard.gba --frames 1800 --seq /tmp/f_
ffmpeg -nostdin -framerate 59.7275 -i /tmp/f_%05d.png \
       -vf "scale=iw*4:ih*4:flags=neighbor" -c:v libx264 -pix_fmt yuv420p -crf 16 out.mp4
```

59.7275 is the GBA's real rate rather than 60 — the core reports it, and using it
keeps the clip the length it claims to be. Check the result with `ffprobe`: a
killed encode leaves a file that is silently short and exits 0.
