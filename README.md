# cartridge

**A console emulator as a live Resolume source.**

`cartridge` is a [libretro](https://www.libretro.com/) frontend that runs inside
Resolume Arena/Avenue as an FFGL generator. Point it at a core and a game and the
console becomes a layer — composite it, key it, run it through other effects, and
MIDI-map the joypad onto whatever controller is already on the desk.

It was built as a companion to
[old-cathode](https://github.com/stoatworks-labs/old-cathode). A 240p Mega Drive
frame through a real composite encoder, with dot crawl and cross-colour that are
consequences rather than decoration, is the thing this exists to make possible.

> **No emulator core, BIOS image or game is included, and none ever will be.**
> You supply your own, exactly as you would with RetroArch.
> **[docs/CORES.md](docs/CORES.md) is how to get them** — where to download,
> which core for which console, and the two macOS traps (architecture matching
> and Gatekeeper quarantine) that otherwise cost you an afternoon.

---

## Two builds, one frontend

The same emulator frontend ships twice, differing only in which process the core
runs in. Which one you want depends on what happens if a core crashes.

|                    | **In process** (the plugin)     | **Out of process** (the helper)      |
| ------------------ | ------------------------------- | ------------------------------------ |
| Latency            | Same frame it was produced      | One composition frame later          |
| A core crashes     | **Resolume goes down with it**  | The helper dies, the show continues  |
| Two of one core    | Needs a private-copy workaround | Free — separate processes            |
| Set up             | Drop the plugin in, pick a file | Run a second program first           |

Rehearsal and studio work: in process. Anything with an audience: the helper.

---

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/cartridge
cd cartridge
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build      # drops the bundle into Resolume's plugin folder
```

## Using it

**In process** — add `Cartridge` as a source, set **Core** to a libretro core
(`.dylib` / `.dll`) and **Content** to a game. That is all.

If the layer stays black, the answer is nearly always in
`~/Library/Logs/cartridge/` — a core built for the wrong architecture or still
carrying a quarantine flag both fail silently otherwise, and the log names which
one and how to fix it.

**Out of process** — start the helper, then point the plugin at it:

```bash
./build/cartridge-helper --core /path/to/mgba_libretro.dylib --content game.gba
```

Set the plugin's **Source** to `Helper` and leave **Channel** at `default`.

## Parameters

**Source**, **Core**, **Content**, **Channel**, **Run**, **Reset**, **Speed**
(¼× to 4×, centred on the console's own rate), **Scaling** (Fit / Fill / Stretch
/ Integer), **Pixel Aspect**, **Smoothing** — plus twelve pad buttons in a
**Controller** group.

The buttons are ordinary boolean parameters, which means Resolume will MIDI-map
them, keyboard-map them and automate them off the timeline with no help from us.
A hardware controller becomes a joypad, and a game can be keyframed as a visual.

**Integer** scaling and **Smoothing** off is the combination you want for pixel
art. **Pixel Aspect** honours the core's declared display aspect — 320×224 on a
Mega Drive is a 4:3 picture, and showing it 1:1 makes it visibly too tall.

## What it does not do

- **Hardware-rendered cores are refused.** Dolphin, Flycast and friends need a
  GL context and an FBO on the emulator thread; giving them a half-kept promise
  makes them render into Resolume's own framebuffer. Software cores only — NES,
  SNES, Mega Drive, Master System, Game Boy, GBA, PC Engine.
- **There is no audio.** FFGL gives a plugin no way to hand audio to the host,
  so the samples are decoded and dropped. Sound from our own device would not be
  on Resolume's clock or in its mixer, which is worse than silence for a show.
- **No core options and no save states** yet.

## Licensing

`cartridge` is MIT. **Emulator cores are not**, and that is why none are bundled:

- **mGBA** is MPL-2.0 — permissive.
- **Genesis Plus GX** is under a **non-commercial** licence.
- Several cores in the Snes9x lineage carry custom non-commercial terms.
- **Dolphin** and **VBA-M** are GPL.

Cores are `dlopen`ed at runtime and never redistributed, so their terms are
between you and their authors — the same arrangement RetroArch uses. Supplying
your own game images is likewise your responsibility.

## Status

**v0.1.0, unreleased.** Verified end to end against a synthetic libretro core
built into the repo (`tools/verify.sh` — CPU host, a real GL context at two
aspects, a real second process, and the helper killed with `SIGKILL` under a
running consumer). **It has not yet been loaded into Resolume, and has not yet
been run against a real emulator core.**

---

*Parts of this project were developed with AI assistance.*

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->
