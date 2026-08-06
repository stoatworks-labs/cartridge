# Getting cores

`cartridge` ships **no emulator cores, no BIOS images and no games**, and never
will. It loads what you point it at, the same way RetroArch does. This page is
how to get them.

---

## The two things that will waste your afternoon

Read these before downloading anything. Both produce a plugin that looks
completely dead, with no message in Resolume.

### 1. The core's architecture must match the *host process*, not your Mac

A universal Mac runs a universal app natively, but a libretro core from the
buildbot is **one architecture only**. If the process loading it is arm64 and
the core is x86_64, `dlopen` refuses it.

What matters is the process doing the loading:

| Mode            | Must match                                        |
| --------------- | ------------------------------------------------- |
| **In Process**  | Resolume's architecture                           |
| **Helper**      | `cartridge-helper`'s architecture                 |

Check Resolume, once:

```bash
lsof -p "$(pgrep -f 'Resolume Arena')" | grep -c -i rosetta
```

`0` means Resolume is running **arm64** natively — download `arm64` cores.
Anything else means it is under Rosetta, and you want `x86_64`.

*(On this machine, checked 2026-08-04: Arena runs native arm64 on an M4 Max, so
it needs arm64 cores.)*

### 2. Downloaded cores are quarantined, and Gatekeeper will block them

Anything downloaded from a browser gets `com.apple.quarantine`, and a
quarantined dylib will not load into a signed, hardened-runtime app. Strip it:

```bash
xattr -dr com.apple.quarantine ~/Documents/Cartridge/cores
```

Note that approving one thing does **not** approve the rest — quarantine is
per-file, so run this again after adding cores.

Resolume itself is fine to load unsigned plugins: it ships with the
`com.apple.security.cs.disable-library-validation` entitlement, which is what
makes FFGL work at all.

---

## Where to download

### Easiest: let RetroArch fetch them

Install [RetroArch](https://www.retroarch.com/), then
**Main Menu → Online Updater → Core Downloader**. It picks the right
architecture for you and handles quarantine. Cores land in:

```
~/Library/Application Support/RetroArch/cores/
```

Point `cartridge` straight at that folder, or copy what you want out of it.

### Direct: the libretro buildbot

<https://buildbot.libretro.com/nightly/>

| Platform                  | Path                                              |
| ------------------------- | ------------------------------------------------- |
| macOS, Apple Silicon      | `nightly/apple/osx/arm64/latest/`                 |
| macOS, Intel              | `nightly/apple/osx/x86_64/latest/`                |
| Windows                   | `nightly/windows/x86_64/latest/`                  |

Files are named `<name>_libretro.dylib.zip` (or `.dll.zip`). Unzip, then strip
quarantine as above.

```bash
mkdir -p ~/Documents/Cartridge/cores && cd ~/Documents/Cartridge/cores
curl -LO https://buildbot.libretro.com/nightly/apple/osx/arm64/latest/mgba_libretro.dylib.zip
unzip -o mgba_libretro.dylib.zip && rm mgba_libretro.dylib.zip
xattr -dr com.apple.quarantine .
```

---

## Which core for which system

**Start with mGBA.** It is MPL-2.0, needs no BIOS, is software-rendered, and
covers Game Boy, Game Boy Color and Game Boy Advance in one file. If something
is wrong with your setup, it is the core least likely to be the cause.

| System                | Core                  | Licence              | BIOS?          |
| --------------------- | --------------------- | -------------------- | -------------- |
| GB / GBC / GBA        | `mgba`                | **MPL-2.0**          | no             |
| GB / GBC              | `gambatte`            | GPLv2                | no             |
| NES                   | `nestopia`            | GPLv2                | no             |
| NES                   | `fceumm`              | GPLv2                | no             |
| NES                   | `mesen`               | GPLv3                | no             |
| SNES                  | `bsnes`               | GPLv3                | no             |
| SNES                  | `snes9x`              | ⚠️ **Non-commercial** | no             |
| Mega Drive / MS / GG  | `genesis_plus_gx`     | ⚠️ **Non-commercial** | only for CD    |
| Mega Drive / 32X      | `picodrive`           | ⚠️ **Non-commercial** | only for CD    |
| PC Engine             | `beetle_pce_fast`     | GPLv2                | only for CD    |

Licences are from the [libretro licence table](https://docs.libretro.com/development/licenses/).

### The non-commercial ones

**Genesis Plus GX, PicoDrive and Snes9x are non-commercial.** Snes9x's own terms
are freeware for personal use, with commercial users directed to ask the
copyright holders first.

That is a live question if you are being paid for the show, or if the output
feeds anything monetised. For Mega Drive content there is no permissive
alternative worth recommending — so if it matters, ask, or pick a different
console. **bsnes** (GPLv3) is the drop-in answer for SNES and avoids the problem
entirely.

None of this is affected by `cartridge` being MIT: cores are `dlopen`ed at
runtime and never redistributed by this project, so their terms are between you
and their authors.

---

## BIOS and system files

Cartridge-based systems in the table above need nothing. **CD-based** ones do —
Mega CD, PC Engine CD. Put the files in a folder and pass it:

```bash
cartridge-helper --core … --content … --system ~/Documents/Cartridge/system
```

In-process, the plugin uses `~/Documents/Cartridge/system` by convention.

When a core rejects your content, **the core's own log line names the missing
file** — and it is forwarded into:

```
~/Library/Logs/cartridge/cartridge.YYYY-MM-DD.log
```

That log is almost always the fastest answer to "why is the layer black".

---

## Games

**Start with the test card in this repo** — `python3 tools/testrom/build.py`
builds a 1 KB GBA ROM of animated test patterns, so you can confirm your core
and your setup work before going looking for anything. It needs mGBA and no
BIOS. See [tools/testrom/README.md](../tools/testrom/README.md).

For actual games: not our department, and not linked from here. Dumping
cartridges you own is the uncontroversial route; there is also a real body of freely-licensed homebrew
(itch.io has a lot of it) which is genuinely well suited to this — it tends to
be bold, high-contrast and built for a 240p raster, which is exactly what
survives being composited and run through
[old-cathode](https://github.com/stoatworks-labs/old-cathode).

---

## Suggested layout

```
~/Documents/Cartridge/
├── cores/      mgba_libretro.dylib, …
├── content/    your games
├── system/     BIOS files, for CD systems
└── saves/      battery saves
```

Nothing enforces this — both file pickers browse anywhere — but it keeps the
`xattr` command above pointed at one place.
