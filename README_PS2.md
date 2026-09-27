# Nazi Zombies: Portable — PlayStation 2 port

This is a native PlayStation 2 port of the Vril engine (NZ:P). It runs the
normal NZ:P QuakeC game code and the handheld (PSP) asset set on the PS2's
Emotion Engine, renders with a purpose-built Graphics Synthesizer backend,
and streams audio to the SPU2 through a small custom IOP module.

Current state: see [PS2_PORT_STATUS.md](PS2_PORT_STATUS.md).

---

## 1. Toolchain: PS2BUILD

The port is built with **PS2BUILD** (TechWritesCode), not the old Docker
`ps2dev` image / `Makefile.eeglobal` layout. The build is described by
[`ps2.yaml`](ps2.yaml) and libraries/IRX modules are resolved from the
installed PS2BUILD packages.

Install PS2BUILD with its official installer for your OS, then keep it
current:

```
ps2build update
```

Useful commands:

```
ps2build schema          # JSON schema of ps2.yaml
ps2build explain         # documentation
ps2build build           # build the project in the current directory
```

With Claude Code, the PS2BUILD MCP server can be attached with
`claude mcp add ps2build -- ps2build mcp` (package lookup, schema, builds).

Packages used (all from the default PS2BUILD suite): `pad`, `mc`, `filexio`,
`patches`, `graph`, `dma`, `packet2`, `draw`, `debug`, `png`, `jpeg`, `z`;
IRX modules embedded with `embed_irx`: `iomanx`, `filexio`, `sio2man`,
`padman`, `mcman`, `mcserv`, `usbd_mini`, `bdm`, `bdmfs_fatfs`,
`usbmass_bd_mini`, `libsd`, plus the project's own `nzpsnd` IOP target.

## 2. Building

```
./tools/ps2/build.sh
```

The script writes `source/_build_info.h` (git hash / branch / build date,
shown in the startup banner and in crash reports) and runs
`ps2build build`. Once that header exists, plain `ps2build build` works too.

Output:

| file | what |
|---|---|
| `build/bin/nzportable.elf` | the game (all IRX modules embedded) |
| `build/bin/nzpsnd.irx` | the audio IOP module (already embedded in the ELF) |
| `build/nzportable.map` | linker map (section sizes, `.bss` audit) |

The build script prints the ELF section sizes after linking. Keep an eye on
`.bss`: static buffers are runtime memory even though they do not grow the
file.

## 3. Game data layout

The ELF finds its data relative to the directory it was started from
(`PS2_GetGameRoot()`), so the installation is:

```
mass:/NZP/                 (or any folder on the USB drive)
    nzportable.elf
    setup.ini              optional: extra command line arguments
    nzp/
        progs.dat
        maps/  gfx/  models/  sounds/  textures/  tracks/  data/ ...
```

Use the **PSP asset set** (`nzportable-psp.zip` from the NZ:P releases, or
`assets/psp` + `assets/common` from `nzp-team/assets`): copy its
`nzportable/nzp` folder next to the ELF. Those assets are sized for a
constrained handheld and are the starting point of the PS2 asset profile
(see *Assets* below). `-basedir <path>` overrides the data root.

## 4. Running

### PCSX2 (development)

1. Enable *Settings → Emulation → Enable Host Filesystem*.
2. Put `nzportable.elf` and the `nzp/` folder in the same directory.
3. Boot the ELF (*System → Start File*, or `pcsx2-qt -elf <path>`).
   `host:` then resolves to the ELF's directory.
4. EE/IOP console output (*Log Window*, or `logs/emulog.txt`) shows the
   startup banner, IOP module list, memory reports and errors.

`setup.ini` next to the ELF can hold arguments, e.g. `+map ndu` to boot
straight into a map.

### Real hardware

1. FAT32/exFAT USB stick, copy `nzportable.elf` + `nzp/` into e.g.
   `mass:/NZP/`.
2. Launch the ELF with any homebrew launcher (wLaunchELF, OPL's app list,
   FMCB/PS2BBL entries...). The IOP is reset and every driver the game
   needs is loaded from inside the ELF — no IRX folder is required.
3. When launched through **ps2link** (`host:` on real hardware), pass
   `-noiopreset`; resetting the IOP would drop the ps2link connection.

Video mode follows the console region (NTSC 640x448 / PAL 640x512,
interlaced, flicker filtered). `-ntsc` / `-pal` force a mode.

## 5. Controls (DualShock 2)

All buttons go through the engine binding system (`bind`), so everything
can be rebound in *Configuration → Controls*. Defaults follow the NZ:P
twin-stick console layout:

| input | game | menus |
|---|---|---|
| left stick | move | navigate (with repeat) |
| right stick | look | — |
| D-pad | quick actions | navigate |
| Cross | jump | confirm |
| Circle | change stance (hold: prone) | back |
| Square | reload (hold: use / buy) | — |
| Triangle | switch weapon | — |
| R1 | fire | — |
| L1 | aim down sights | — |
| R2 / L2 | grenade / secondary grenade | — |
| L3 | sprint | — |
| R3 | knife | — |
| Start | pause menu | — |

`in_ps2_deadzone` (default 0.10) sets the hardware stick deadzone; the
shared `in_tolerance` / `in_acceleration` / `sensitivity` cvars shape the
response. Vibration is supported (`in_rumble`).

## 6. Architecture

### Platform (`source/platform/ps2/`)

| file | role |
|---|---|
| `ps2_main.c` | entry point, IOP bring-up, hunk allocation, main loop, memory checkpoints |
| `ps2_iop.c` | IOP reset, `sbv` patches, embedded IRX table (per-service groups), device wait |
| `ps2_system.c` | `Sys_*`: POSIX file layer (fileXio backed), timers, fatal error screen, game root |
| `ps2_mem.c` | memory accounting, reports, OOM interception (`--wrap=malloc`...) |
| `ps2_input.c` | DualShock 2 via libpad: state machine, analog lock, both sticks, rumble, menu stick |
| `ps2_sound.c` | Quake `SNDDMA_*` backend → `nzpsnd.irx` |
| `iop/nzpsnd/` | IOP PCM streamer (SIF RPC → ring → SPU2 core 0 ADMA via libsd) |
| `ps2_profile.c` | frame profiler (`ps2_profile 1`) |
| `ps2_network.c`, `net_*.c` | loopback + datagram layer (no LAN driver yet) |
| `music.c` | background music (not implemented yet, see status) |

### Renderer (`source/platform/ps2/gs/`)

The renderer was derived from the PSP GU renderer (the most memory
conscious NZ:P renderer; it already keeps textures indexed with CLUTs and
drives the shared `Hyena` interface), with every GU call replaced by a
small purpose-built GS layer, **`gs_core.c`**. Alternatives considered:

* **PS2GL** — not provided by the installed PS2BUILD packages, and an
  OpenGL emulation layer is a poor fit for this renderer's needs.
* **gsKit/dmaKit** — mature, but allocate large internal queues, own VRAM
  allocation and do not help with the actual work (transform/clip).
* **Native GS layer (chosen)** — only what the renderer uses: matrix stack,
  draw arrays, a handful of states, texture residency.

`gs_core.c` in short:

* **Transform & clip on the EE.** Vertices are transformed to clip space,
  trivially accepted/rejected with outcodes and polygon-clipped (near plane
  + guard band) only when needed, then projected to 12.4 fixed point.
  Backface culling happens here too.
* **DMA.** Two fixed 128 KiB source-chain buffers. The EE fills one while
  the GIF DMA channel consumes the other; each GS primitive batch is a
  REGLIST GIF packet preceded by an A+D `PRIM` write (the GIF ignores the
  PRIM field of REGLIST tags). Nothing is allocated per frame.
* **Textures are never copied into the command stream.** Uploads are DMA
  REF tags pointing at the EE-side texel data (written back with
  `SyncDCache` first).
* **VRAM texture ring.** Texture VRAM is a FIFO ring: a texture is uploaded
  when bound and not resident; allocating wraps around and evicts the oldest
  textures. Because the GIF stream is strictly ordered, evicting a texture
  used earlier in the same frame is safe. `ps2_vram` shows the numbers.
* **Depth** is stored inverted (`0xffff - z`) because the GS only has
  GEQUAL/GREATER tests.
* **Lightmaps.** The GS blend unit cannot multiply two colours, but it can
  compute `Cd * As`. Lightmaps are stored as 8 bit planes uploaded as PSMT8
  with a ramp CLUT (index → alpha), and a second pass blends
  `((Cd − 0) · As) >> 7`, which reproduces the PSP's 2× overbright
  modulate exactly. Coloured lighting uses three passes with the frame
  buffer write mask limited to one channel each; `r_ps2_colorlightmaps`
  (0 luminance, 1 colour, 2 auto by memory, default 2).

VRAM budget (NTSC):

| | KiB |
|---|---|
| 2 × 640×448 PSMCT16 frame buffers | 1120 |
| 640×448 PSMZ16 depth buffer | 560 |
| texture ring (textures + CLUTs + lightmaps) | 2416 |

`-z24` switches to a 24 bit depth buffer (costs 560 KiB of texture space).

Texture formats (EE backing store, `gs_texture.cpp`):

| source | GS format |
|---|---|
| Quake palette 8 bit (gfx.wad, sprites...) | PSMT8 + shared palette CLUT |
| WAD3 world textures / skins with own palette | PSMT8 + own CLUT (PSP used 4 bit; `r_ps2_worldtex4bit 1` restores that) |
| true colour, opaque or 1 bit alpha | PSMCT16 |
| true colour with translucency | PSMCT32 |

## 7. Memory budget

The EE has 32 MiB (31 MiB usable). Target: **≤ 27–28 MiB peak**, leaving
several MiB for transients. The engine hunk is 12 MiB (`-heap <MiB>` in
`setup.ini` to change).

Measured in PCSX2 on `nzp_warehouse` after spawning (see the
`PS2 MEMORY` reports in the log):

| | MiB |
|---|---|
| ELF image (text 818 K, data 361 K, bss 2.3 M) | 3.43 |
| engine hunk (12.0 reserved, 5.5 used: QuakeC 2.2, map 0.7, sound cache 3.8 of the rest) | 12.0 |
| textures (EE backing) | 4.4 |
| lightmaps | 0.1 |
| other malloc | ~0.3 |
| **EE free** | **10.7** |

Reports are printed automatically at startup, before/after map load, after
spawning and after unload; `ps2_mem` prints one at any time, `ps2_vram`
prints the VRAM ring. An allocation failure prints the full report with the
failing request before the fatal error screen.

IOP: embedded modules total ~234 KiB of the 2 MiB IOP RAM; `nzpsnd` uses
~57 KiB of IOP RAM at run time (40 KiB ring).

## 8. Debugging

* Startup log: build hash, boot path, IOP module list, video mode, audio
  and controller initialisation, memory report.
* `-developer` / `-ps2debug`: more verbose diagnostics (largest hunk blocks
  in memory reports, `Con_DPrintf`).
* `ps2_profile 1`: every 2 s prints average ms per frame section plus DMA
  wait, vsync wait, vertices, texture uploads and DMA kicks.
* `r_ps2_vsyncinterval` (default 2 = 30 fps at 60 Hz).
* Fatal errors show the message and a memory summary on screen
  (debug text mode) and on the EE console.

## 9. Assets

The PS2 currently runs the PSP asset profile unchanged. Planned PS2 asset
profile (`common` + PSP/handheld + PS2 overrides, converted offline):

* 8 bit indexed versions of true colour images (halves EE/VRAM use vs.
  PSMCT16, quarters vs. PSMCT32),
* music converted to SPU2 ADPCM for hardware-decoded streaming,
* a PS2 `config.cfg` with the DualShock 2 bindings above.

## 10. Current limitations

See [PS2_PORT_STATUS.md](PS2_PORT_STATUS.md) for the live list. Notably:
no background music yet, no network play, settings are written next to the
game data (memory card saving is planned), and everything has so far been
validated in PCSX2 only — real hardware testing is still required.
