# cartridge

A **libretro emulator core as a live Resolume source** — a console game becomes a
layer you can composite, key, MIDI-map and run through other effects. Ships in
two builds from one frontend: an FFGL plugin that runs the core **in process**
(lowest latency, a core crash takes Resolume with it) and a **helper process**
that publishes through shared memory (one frame slower, survives a core crash).

C++17, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. Public MIT.

Read `AGENTS.md` before changing the transport, the threading, or anything about
which cores may be shipped. **No core, BIOS or ROM is ever committed here.**

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install the plugin to Resolume: `cmake --install build`
- Skip pieces: `-DCARTRIDGE_BUILD_PLUGIN=OFF`, `-DCARTRIDGE_BUILD_HELPER=OFF`,
  `-DCARTRIDGE_BUILD_TOOLS=OFF`

## Verify
- Everything: `tools/verify.sh`
- The libretro host, no ROM needed: `./build/cartest --check`
- The RGB565 path: `CARTRIDGE_TESTCORE_FORMAT=565 ./build/cartest --check`
- The real plugin in a real GL context: `./build/cargl --check`
- The other letterbox branch: `./build/cargl --check --size 720x720`
- A frame as a PNG: `./build/cartest --frames 20 --press 8 --out /tmp/f.png`
- What a core says about itself: `./build/cartest --core PATH --info`
- Footage from a real core: a joypad timeline plus a frame per PNG —
  `./build/cartest --core PATH --script cues.txt --frames 1800 --seq /tmp/f_`
  then `ffmpeg -nostdin -framerate 60 -i /tmp/f_%05d.png …`. `--press` holds one
  button for the whole run, which cannot get a game off its title screen;
  `--script` is `FRAME ID[,ID...]` per line, or `-` to release everything.

## Running the helper
- `./build/cartridge-helper --core PATH [--content PATH] [--channel NAME]`
- Then set the plugin's **Source** to `Helper` and **Channel** to the same name.
- Two helpers need two channel names.
- `--system DIR` / `--save DIR` are handed to the core for BIOS and saves.
- Check it end to end without Resolume: `./build/cargl --check --helper NAME`
- Prove crash survival: `./build/cargl --helper NAME --survive 6`, and
  `kill -9` the helper partway through.

## Parameters
`Source` (In Process / Helper), `Core`, `Content`, `Channel`, `Run`, `Reset`,
`Speed`, `Scaling` (Fit / Fill / Stretch / Integer), `Pixel Aspect`, `Smoothing`,
then twelve pad buttons in a **Controller** group — plain booleans, so Resolume
MIDI-maps, keyboard-maps and automates them like anything else.

## Notes
- **The core runs on its own thread at its own rate**, not from `ProcessOpenGL`.
  Otherwise emulation speed is the host's frame rate and a heavy show slows the
  game down. See AGENTS.md.
- **libretro callbacks have no user pointer**, so routing is a `thread_local` set
  around every entry into the core — one thread at a time per `Core`, always.
- **Two instances of one core share globals** unless loaded from unique paths.
  The plugin passes `uniqueInstance`; the helper does not need to.
- **`pitch` is not `width * bpp`.** Cores allocate at max geometry.
- All host parameters are 0..1 and mapped in `Controls.h`; `SetParamInfo` clamps
  a standard default before `SetParamRange` can widen it. **Option parameters are
  the exception** — they hold the element value.
- **No FBO anywhere**, and no `ffglex::Scoped*` bindings — both are SDK traps.
- **Hardware-rendered cores are refused**, deliberately and with a log line.
  Software cores only: NES, SNES, Mega Drive, Game Boy, GBA, PC Engine.
- **There is no audio output.** FFGL has no audio path; samples reach `AudioRing`
  and stop.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics
`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It covers the failures that all look identical from outside ("the
layer is black"): a core that would not load, content the core rejected, a core
asking for hardware rendering and being refused, a helper that stopped
responding, and the core's own log forwarded through `GET_LOG_INTERFACE` — which
is usually the only thing that names a missing BIOS file.

    ~/Library/Logs/cartridge/cartridge.YYYY-MM-DD.log
