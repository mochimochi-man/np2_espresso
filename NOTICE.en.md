# np2_espresso — Third-Party License Notice (NOTICE)

np2_espresso is a port of the PC-9801 (VM21-class / V30) emulator NP2kai to the
ESP32-S3. This distribution includes the third-party code listed below. The
copyright of each work belongs to its respective author(s), and each is
redistributed under its own license terms.

------------------------------------------------------------------------------

## 1. NP2kai / Neko Project II · 21/W (emulator core)

- Location: `components/np2kai/`
  (i286c core, io, mem, vram, fdd, font, generic, lio, bios, cbus, codecnv,
   common, trap, diskimage, and the native sound engines under sound/:
   opngen/OPNA, PSG, beep, ADPCM, PCM86, TMS3631, rhythm, CS4231, CT1741, OPL3, etc.)
- License: mostly the **Modified BSD License (3-clause BSD)**.
- A few peripheral components use other licenses; see
  `components/np2kai/np2kai/LICENSES/` for details.
- The built-in font (`font/fontdata`) is the NP2 author's own original ANK
  substitute font — it is NOT a CGROM dump from real (NEC/EPSON) hardware.
  Modified BSD.
- Upstream: https://github.com/AZO234/NP2kai / https://simk98.github.io/np21w/

## 2. TFT_eSPI (display driver library)

- Location: `components/TFT_eSPI/`
- Copyright: Copyright (c) Bodmer
- License: **FreeBSD License (2-clause BSD equivalent)** — see
  `components/TFT_eSPI/LICENSE`
- Upstream: https://github.com/Bodmer/TFT_eSPI

## 3. np2_espresso ESP32 port code (this project's own work)

- Location: `main/`, the various `CMakeLists.txt`, `sdkconfig.defaults`,
  build scripts, etc.
- License: see the root `LICENSE` file.

------------------------------------------------------------------------------

## Components removed from this distribution

To simplify distribution and licensing, the following code, which is not used by
the build, was physically removed. (None of it is used by this emulator's build
configuration: the i286c core + native sound engines.)

- `sound/fmgen/` — cisc's FM sound core (cisc's own license)
- `sound/mame/` — MAME OPL (**GPL**)
- `sound/mamebsd/`, `sound/mamebsdsub/` — ymfm (3-clause BSD)
- `sound/vermouth/` — GM/MIDI software synthesizer
- `i386c/` — the entire IA-32 (386) core. Included DOSBox-derived FPU code
  (**GPLv2**) and Berkeley SoftFloat (BSD)
- `sdl/cmmidi.c` — MIDI output communication
- The now-orphaned license documents under `LICENSES/` that corresponded to the
  above

> As a result, the built binary and the primary sources consist solely of
> permissively licensed code (BSD family). **GPL code has been removed from the
> distribution.**

## Note (unused subtrees that remain)

Under `components/np2kai/np2kai/` the following NP2kai-derived subtrees remain,
although they are unused by the ESP32 build (all excluded from the build). Each
has its own license; refer to the corresponding file under `LICENSES/`. They can
be removed as well if desired.

- `wab/` (Cirrus GD54xx / TGUI9680 VGA), `windows/`, `wx/`, `x/` (per-OS GUIs),
  `network/` (LGY-98, etc.), `embed/`, `np2tool/`, `romimage/`, `textnorm/`,
  `sdl/` (excluding cmmidi.c)
