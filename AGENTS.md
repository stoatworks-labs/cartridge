# cartridge — orientation for another LLM (or a newcomer)

**What it is:** a **libretro frontend that lives inside Resolume**. A console
emulator core becomes an FFGL source, so a game is a live layer you can composite,
key, MIDI-map and run through other effects. C++17, CMake, universal macOS
`.bundle` + Windows `.dll`. Public, MIT, `github.com/stoatworks-labs/cartridge`.

It ships in **two builds that are the same frontend**, differing only in which
process the emulator runs in:

- **In process** (`Cartridge.bundle`) — the core runs inside Resolume. Lowest
  possible latency: the frame is drawn the same composition frame it was
  produced. **A core that crashes takes Resolume with it.**
- **Out of process** (`cartridge-helper`) — the core runs in its own process and
  publishes frames through shared memory. Costs one frame and a 286 KB copy.
  **A core that crashes crashes the helper, and the show keeps running.**

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the transport, the threading, or anything to do
with licensing.

---

## The one idea

**The emulator's clock and the composition's clock are separate facts, and the
frontend is the seam between them.**

A libretro core runs at 60.0988 Hz (NTSC), 59.7275 (Game Boy) or exactly 50
(PAL). Resolume runs at whatever the composition is set to, minus whatever the
show is costing it tonight. Driving `retro_run` from `ProcessOpenGL` would weld
the two together, and the result is a Mega Drive that slows down when the
projection load goes up — the same class of failure orrery avoids by refusing to
integrate velocity, except here it changes the pitch of the music as well.

So the core gets **its own thread at its own rate**, and the renderer samples the
latest finished frame through a triple buffer. Everything else follows:

- 50 Hz content works at all, instead of running 20% fast.
- A frame-rate drop in Resolume is a dropped frame, not a slower game.
- Speed becomes a parameter rather than an accident.
- The harness can step the core synchronously with no thread and no clock, which
  is what makes every test in `cartest` deterministic.

---

## The traps

Ordered by how much time they will cost you.

**libretro callbacks carry no user pointer.** Every callback in the ABI is a bare
function pointer — `void retro_set_video_refresh( retro_video_refresh_t )` — with
nowhere to put a `this`. The C API assumes a frontend hosts exactly one core,
which is true of RetroArch and false of a plugin a VJ can drop on four layers.
Routing here is a `thread_local` current-instance pointer set by an RAII guard
around **every** entry into the core. It works because a `Core` is only ever
called from one thread at a time. Break that and it silently mis-routes — frames
from one emulator appearing on another layer.

**Two instances of the same core in one process share state.** `dlopen` of a path
already loaded returns the *same* handle with a bumped refcount, not a second
copy. Two `Core` objects on one `.dylib` get the same emulated CPU and the same
framebuffer; the second `retro_init` walks over the first. `RTLD_LOCAL` does not
help — it scopes symbol visibility, not the library instance. The fix is to copy
the core to a uniquely-named temp file and open *that*, because dlopen keys on
path (`Core::Load( …, uniqueInstance )`). It is the only in-process answer that
exists. The robust answer is the helper, where separate processes have separate
globals by construction.

**`GET_VARIABLE` must return false, not true with a null value.** Core options
are not exposed, so there is never a value to hand back. Both readings of the
header are defensible; only one is safe. A great many cores are written as
`if( environ_cb( GET_VARIABLE, &var ) ) strcmp( var.value, "on" );` and
dereference the null the instant the call says true. fceumm and Genesis Plus GX
both segfault that way *inside `retro_load_game`*, which from the outside is
indistinguishable from a malformed ROM — the bisection went through the iNES
header byte by byte before suspecting the frontend. Answering false makes every
core fall back to its own defaults.

**A core handed a null system directory does not degrade gracefully.** Four
cores fail four different ways: fceumm and Genesis Plus GX segfault, Nestopia
and Mesen return false from `retro_load_game`. `Core` has always had
`SetSystemDirectory`; nothing called it. `cartest` now takes `--system` and
`--save` and defaults both to the content's own directory.

**`pitch` is not `width * bytesPerPixel`.** Cores hand over a framebuffer
allocated at the console's *maximum* geometry and only partly filled, so rows are
routinely wider than the picture. Walking it as `width * bpp` gives a picture
that shears further to one side on every row. Every real core does this, which is
why the synthetic test core does it too — deliberately publishing at max width so
the trap is covered by the normal suite rather than by a special case.

**The X in XRGB8888 is undefined, not zero.** Several cores leave stale bits
there. Copying it through gives Resolume a mostly-transparent layer, which reads
as "the plugin does nothing" against a black composition. Alpha is forced to 255.

**Three conflicting ideas of row 0.** libretro software framebuffers are
top-left origin; GL textures (and FFGL) are bottom-left; PNG is top-left. The
picture is upside down in exactly one of the three places you look if any one is
wrong. `pixels::Convert` flips once, so `Frame::pixels` is bottom-up and goes
straight into a texture; `png::WritePng` and `png::Sample` un-flip. Nothing else
in the repo thinks about it.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. There is no `SetParamDefault`. So every host parameter
is 0..1 and the conversions live in `Controls.h` — Speed is exponential and
centred so mid-travel is exactly 1.0x.

**Option parameters do NOT hold 0..1.** They hold the element value the operator
chose — 0, 1, 2 — read through a rounding clamp so a composition saved against a
build with more elements cannot index off the end.

**An unhandled TEXT or FILE parameter kills the plugin on an instantiate sweep.**
`SetTextParameter` handles `PT_CORE`, `PT_CONTENT` and `PT_CHANNEL` and returns
`FF_FAIL` for anything else rather than falling through.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** The render path uses plain `glUseProgram` and `glBindTexture` and puts
state back by hand. **No FBO is allocated anywhere**: `FFGLFBO::Initialise`
allocates under a `ScopedTextureBinding` whose destructor clears the binding, and
`FFGLFBO::Release` leaks its colour texture. A source draws a textured quad and
needs neither.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that will not compile surfaces only at runtime as
"the plugin does nothing". That is what `Diag` and `cargl` are for.

**A POSIX shared memory object can only be sized once.** A helper killed with
SIGKILL — which is what a crashing core looks like — never unlinks, so its
segment outlives it. Opening that with `O_CREAT` succeeds and `ftruncate` then
fails with `EINVAL` on macOS, leaving a mapping of `sizeof(Block)` over a smaller
object: every access past the old end faults. Invisible until the first build
that adds a field. `shared::Open` unlinks before creating, and creates with
`O_EXCL`.

**macOS caps a POSIX shm name at 31 characters including the leading slash**, and
fails with `ENAMETOOLONG` rather than truncating. `ChannelPath` builds and clamps
it in one place.

**No locks in shared memory, ever.** A mutex held by a helper that crashed is a
plugin that hangs Resolume forever — precisely the failure the out-of-process
build exists to prevent. Every cross-process field is a lock-free atomic, with a
`static_assert` to keep it that way.

**App Nap will demote the emulator thread.** A covered window was enough to take
a worker thread from 50 to 7 fps elsewhere in the fleet, and a dedicated thread
was not sufficient on its own. Both the runner thread and the helper's publish
loop ask for `QOS_CLASS_USER_INTERACTIVE`.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `cartridge_core` and `cartridge_plugin` are
**OBJECT** libraries and `SourcePlugin.cpp` is listed directly in the MODULE
target. Related: CMake does not propagate the *objects* of one OBJECT library
through another, so final targets name both explicitly.

---

## Licensing — read this before adding a core

**No core, no BIOS and no content is shipped with this repo, ever.** The plugin
loads what the operator points it at, and that is a deliberate legal position,
not an oversight:

- **mGBA is MPL-2.0** — permissive, fine.
- **Genesis Plus GX is under a non-commercial licence.**
- Several others in the Snes9x lineage carry custom non-commercial terms.
- Dolphin and VBA-M are GPL.

Linking any of those into a distributed binary would take this repo's MIT licence
with it, and the non-commercial ones interact badly with a project that has a
Patreon. The arrangement here is RetroArch's: `dlopen` at runtime, ship nothing,
and the core's terms bind the operator rather than us. `.gitignore` blocks
`cores/`, `roms/`, `system/` and loose shared libraries for the same reason.

If anyone ever proposes bundling a core "just to make it easier to try", the
answer is a link to this section.

---

## Checking your work

`tools/verify.sh` runs the lot, in three layers that fail for different reasons.

- **`cartest`** — the libretro host on the CPU. Runs against a **synthetic core
  built into this repo** (`tools/testcore`), which is why it can assert exact
  pixel coordinates and exact sample values with no ROM anywhere. Every part of
  that core's test pattern pins one specific frontend mistake: corner primaries
  catch a channel swap and the flip, a bar whose x position *is* the frame
  counter catches a stale slot and double-stepping, a centre block catches the
  input path, a padded pitch catches the pitch trap, and a square wave catches
  the audio ring. It also checks determinism (two cold runs, byte-identical) and
  that two instances of one core keep separate state.
- **`cargl`** — the **real plugin class** through the real FFGL sequence in a
  headless CGL 4.1 core context. The only thing that catches a shader that will
  not compile or a uniform that does not resolve. Run at **two aspects**: a sign
  error in the fit branch is invisible whenever the picture happens to be wider
  than the frame, and a square render is the cheapest way to make the other
  branch matter. (The first version of this check hardcoded pillarboxing and
  failed at 1:1 for reasons that had nothing to do with the plugin.)
- **The helper section** — the same `cargl` checks across a real process
  boundary, then the helper **killed with SIGKILL** underneath a running
  consumer, asserting the picture is still there afterwards. That is the claim
  the whole out-of-process build exists to make, so it is tested rather than
  stated.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from a
session is unreliable. **Nothing in this repo has been loaded into Resolume
yet**, and neither has any real emulator core — everything above runs against the
synthetic one. The three things most worth checking first are: how the parameter
groups land in the inspector, whether a controller MIDI-maps onto the pad
usefully, and whether a real core (mGBA is the friendliest starting point) loads
and runs.

---

## Things deliberately not done

- **No hardware-rendered cores.** `RETRO_ENVIRONMENT_SET_HW_RENDER` is
  explicitly refused, and the refusal is logged. Accepting it means promising a
  GL context, an FBO and a `get_current_framebuffer` callback on the emulator
  thread — and a hardware core given a half-kept promise does not fail cleanly,
  it renders into whatever is bound, which in the plugin build is Resolume's own
  framebuffer. Doing it properly means a second GL context shared with the host,
  and the cores that need it (Dolphin, Flycast) are also the ones whose macOS
  support is weakest. Software cores are NES, SNES, Mega Drive, Master System,
  Game Boy, GBA, PC Engine — which is precisely the content worth putting
  through **old-cathode**, so the easy half is the half that matters.
- **No audio output.** FFGL has no audio path — a Resolume plugin returns a
  texture and nothing else. The core's samples land in `AudioRing` and stop
  there. Opening our own device would produce sound that is not on Resolume's
  clock and not in Resolume's mixer, which is worse than silence for a show. The
  ring exists so that a helper *can* grow a device later without touching the
  core.
- **No zero-copy GPU transport.** A console frame is 320×224×4 = 286 KB and
  copying it costs about 20 µs against a 16.7 ms budget. An IOSurface or Spout
  path would buy back a tenth of one percent in exchange for a macOS-only,
  framework-dependent transport that two headless processes could not test.
  `shared::kMaxWidth`/`kMaxHeight` cap the channel at 1280×960, which is roughly
  where the copy stops being free — a core past that is **refused with a
  message** rather than silently dropping frames.
- **No core options.** `GET_VARIABLE` answers "no value set", which every core
  must cope with because it is what a first run looks like. Exposing them means
  a dynamic parameter list, and FFGL's is fixed at construction.
- **No save states.** They are the obvious next feature and they need a file
  picker, a slot concept and a policy on what happens when the composition
  reloads.

Related: [old-cathode](https://github.com/stoatworks-labs/old-cathode) (the
intended downstream — a real console through a real composite signal path),
[nesolume](https://github.com/stoatworks-labs/nesolume) (the simulated console
this is the genuine article for), orrery (the CMake, Diag and harness patterns
came from there), downpour, resolume-luma-keyer.
