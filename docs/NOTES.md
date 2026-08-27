# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*cartridge — a libretro emulator core as an FFGL source for Resolume; TWO builds (in-process plugin + out-of-process helper) from one frontend. Companion to old-cathode. PUBLIC MIT, released v0.1.0 macOS. Has run real third-party cores; still never loaded into Resolume and no console emulator run*

**cartridge** (started 2026-08-04) — a **libretro frontend that runs inside
Resolume** as an FFGL `FF_SOURCE`. A console emulator becomes a live layer.
`~/Projects/cartridge`, intended **PUBLIC MIT**, v0.1.0.

**Status: PUBLIC, released v0.1.0 (2026-08-06), macOS universal only.** On the
website with a project page and in the video-plugins suite as "The console".
Windows and Linux branches exist in the source and neither has ever been built.

**It HAS now been run against real third-party cores** (2026-08-06): `2048` and
`gong` from the libretro buildbot both load, negotiate `SET_SUPPORT_NO_GAME` and
render through the plugin's own frame path; 2048 was played to score 1216 by a
scripted joypad. **mGBA is installed too** (arm64,
`~/Documents/Cartridge/cores/`) and loads, but **needs a ROM nobody has supplied**
— so **no console emulator has been run**, and no ROM/BIOS/save path is
exercised. Still **never loaded into Resolume**.

**Getting footage from a real core:** `cartest --script cues.txt --frames N --seq
PREFIX_` then ffmpeg. `--press` holds ONE button for the whole run and cannot get
a game off its title screen — that gap is why `--script` (a `FRAME ID[,ID...]`
timeline) and `--seq` were added on 2026-08-06.

**Verified in Resolume only as far as: Arena loaded the bundle and CONSTRUCTED
the plugin** (survived the instantiate/parameter-enumeration sweep, the trap an
unhandled TEXT/FILE param turns into a crash). Never rendered on a layer, and
**never run against a real emulator core**.

**Arena on this Mac runs NATIVE arm64** (M4 Max, checked 2026-08-05) and carries
`com.apple.security.cs.disable-library-validation` — which is why unsigned FFGL
plugins load at all. So it needs **arm64** cores.

**Do NOT drive the Resolume GUI with synthesized input.** Two clicks on
category triangles simply did not register (custom-drawn UI), and synthesized
*keystrokes* went to the composition as clip triggers — this modified Allan's
live `boxpark` project. See **screenshot capture** (working-practice note, kept in Claude memory) and
**cosession shared checkout** (working-practice note, kept in Claude memory).

**Two builds, one frontend, and the choice is about crash blast radius:**
- **In process** (`Cartridge.bundle`) — lowest latency, **a core crash takes
  Resolume down**.
- **Out of process** (`cartridge-helper`) — POSIX shm transport, one frame
  slower, survives a core crash. Also the only clean way to run two instances
  of one core.

**Why it pairs with [old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`):** software-rendered cores are NES /
SNES / Mega Drive / Game Boy / GBA / PC Engine — exactly the 240p content worth
putting through a real composite signal path. [nesolume](https://github.com/stoatworks-labs/nesolume/blob/main/docs/NOTES.md) (`nesolume`) simulates a
console; this is the genuine article.

**The traps that cost the most, all documented in the repo's AGENTS.md** (see
[agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md)):
- **libretro callbacks carry no user pointer** — routed via a `thread_local`
  set by an RAII guard around every entry; one thread at a time per Core or it
  silently mis-routes frames between layers.
- **`dlopen` of an already-loaded path returns the SAME handle**, so two
  instances of one core share globals. Fixed by copying the core to a unique
  temp path. `RTLD_LOCAL` does not help.
- **`pitch` is not `width * bpp`** — cores allocate at max geometry; the
  synthetic test core publishes a padded pitch deliberately.
- **A POSIX shm object can only be sized once** — a SIGKILLed helper leaves its
  segment, `ftruncate` then fails EINVAL on macOS and the oversized mapping
  SIGBUSes. `shared::Open` unlinks before creating, with `O_EXCL`. Latent until
  the struct grows.
- **The X in XRGB8888 is undefined, not zero** — pass it through and Resolume
  gets a transparent layer.
- Three conflicting ideas of row 0 (libretro top-left, GL/FFGL bottom-left, PNG
  top-left); flipped once in `pixels::Convert`.

**Hardware-rendered cores (`SET_HW_RENDER`) are deliberately REFUSED** — a
half-kept promise makes the core render into Resolume's own framebuffer.
**There is no audio**: FFGL has no audio path at all.

**Verification is `tools/verify.sh`, three layers, all passing:** CPU host
(`cartest`, incl. determinism + two-instance separation), the real plugin class
in a headless CGL 4.1 context (`cargl`, run at **two aspects** — a fit-branch
sign error is invisible at 16:9 only), and the helper across a real process
boundary **then SIGKILLed under a running consumer**.

**Licensing is load-bearing and non-obvious:** ships no cores/BIOS/ROMs ever.
**Genesis Plus GX is non-commercial**, Snes9x lineage often is too; mGBA is
MPL-2.0 (the friendliest first real core to try); Dolphin/VBA-M are GPL.
Bundling any would take the MIT licence with it — matters given Patreon. See
[licence gaps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/project_licence_gaps.md), **disclaimer scope** (working-practice note, kept in Claude memory) (disclaimer present).

Related: [orrery](https://github.com/stoatworks-labs/orrery/blob/main/docs/NOTES.md) (`orrery`) (CMake/Diag/harness patterns came from there),
[ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md), [macos app nap](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_macos_app_nap.md) (both threads request
`QOS_CLASS_USER_INTERACTIVE`).

## 2026-08-22: v0.1.1, released from `main`

The About block release. Note the branch hazard: this repo's working checkout
sits on `cartest-system-directory` (open PR #1, the GET_VARIABLE fix), so the
release work was done on `main` in a throwaway worktree and the PR branch left
alone. **v0.1.1 therefore ships WITHOUT the GET_VARIABLE fix** — that still
waits on PR #1.

The repo has **no CI workflows at all**, so its releases are built by hand; see
[fleet mass release traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_fleet_mass_release_traps.md) for the shape, including the
`cartridge-helper` signing gap.
