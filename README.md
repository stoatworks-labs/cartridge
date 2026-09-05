# cartridge

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The macOS build is
> verified end to end: against a synthetic libretro core, against real
> third-party cores from the buildbot, against mGBA running a real GBA cartridge
> image for 1800 consecutive frames, and inside Resolume Arena 7.27.1 with three
> cores swapped live on a running layer. **The Windows and Linux branches have
> never been built or tested**, and no binary is published for either. Whether a
> controller MIDI-maps onto the pad is **unconfirmed** — check it in your own rig
> before a show depends on it.

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

No game to hand? `python3 tools/testrom/build.py` builds a 1 KB GBA test card —
animated bars, crosshatch, a sweeping edge and a checkerboard — so you can prove
the path works without obtaining anything. It ships as source, not as a ROM.

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

<!-- downloads:start -->

## Download

**[v0.1.2](https://github.com/stoatworks-labs/cartridge/releases/tag/v0.1.2)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`cartridge-0.1.2-macos-universal.dmg`](https://github.com/stoatworks-labs/cartridge/releases/download/v0.1.2/cartridge-0.1.2-macos-universal.dmg) | 549 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`cartridge-0.1.2-macos-universal.zip`](https://github.com/stoatworks-labs/cartridge/releases/download/v0.1.2/cartridge-0.1.2-macos-universal.zip) | 270 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .zip archive | [`cartridge-0.1.2-windows-x86_64.zip`](https://github.com/stoatworks-labs/cartridge/releases/download/v0.1.2/cartridge-0.1.2-windows-x86_64.zip) | 172 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/cartridge/releases](https://github.com/stoatworks-labs/cartridge/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## Status

**v0.1.1, released for macOS** (2026-08-22). The released binary predates the GET_VARIABLE fix (777dd40), which is on `main` but not in that tag. Verified end to end against a synthetic libretro
core built into the repo (`tools/verify.sh` — CPU host, a real GL context at two
aspects, a real second process, and the helper killed with `SIGKILL` under a
running consumer).

**It has since been run against real third-party cores.** `2048` and `gong` from
the libretro buildbot both load, negotiate `SET_SUPPORT_NO_GAME`, and produce
frames through the same path the plugin uses; 2048 was played to a score of 1216
by a scripted joypad (`--script`), which exercises input, the pixel format, the
padded pitch and the triple buffer against code this project did not write.

**A real console emulator has now been run too.** mGBA loads a GBA cartridge
image supplied locally and runs it at the console's own 59.7275 Hz, with the
core's log forwarded — including its own SRAM save-type detection — and 1800
consecutive frames rendered without a drop. That exercises the emulator path
proper: a real ROM, a real save type, and a core an order of magnitude larger
than the ones above. No ROM is included in this repository or distributed with
it, and none ever will be.

**And it has now run inside Resolume.** In Arena 7.27.1 on macOS: added to a
layer as a source, `InitGL` on the host's own GL 4.1 Metal context, and three
cores loaded and swapped live on a running layer — 2048, the repo's test core,
and mGBA. The parameter groups land in Arena's inspector as declared: Source as
an In Process / Helper pair, Core and Content as file pickers that open in
`~/Documents/Cartridge`, Scaling as Fit / Fill / Stretch / Integer, and the
twelve pad buttons in their own Controller group. Picking mGBA with no ROM
selected produced `content load failed: core mGBA requires content` in the log
rather than a silent black layer, which is what that diagnostic is for.

One thing is still unconfirmed, and it needs hardware rather than a session:

- **Whether a controller actually MIDI-maps onto the pad.** The buttons are
  ordinary boolean parameters, so Resolume should map them like any other, but
  nobody has put a real controller on them. Check it in your own rig before a
  show depends on it.

The macOS build is universal (Apple Silicon and Intel), Developer ID-signed and
notarised, so it opens with no Gatekeeper step. The source carries Windows and
Linux branches; **neither has been built or tested**, and no binary is published
for either.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->
