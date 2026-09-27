# PS2 port status

Validated in PCSX2 (hardware and software GS renderers) unless noted.
**Nothing has been verified on a real PlayStation 2 yet.**

```
Build (PS2BUILD)   WORKING   ps2build build -> build/bin/nzportable.elf
Boot / IOP         WORKING   IOP reset, 12 embedded IRX modules
Filesystem         WORKING   host: (PCSX2); mass: USB path implemented, untested on HW
Controller         WORKING   DualShock 2, both sticks, menu navigation; rumble untested
2D GS              WORKING   menus, fonts, pictures, HUD panels
3D GS              PARTIAL   world + lightmaps render; view model not visible yet
Textures           WORKING   T8/T4 + CLUT, CT16/CT32, VRAM ring cache
Lightmaps          WORKING   alpha-multiply pass (luminance / colour)
BSP                WORKING   HL BSP v30 maps load (nzp_warehouse, ndu)
Models             PARTIAL   alias models decode correctly; lighting/visibility checks pending
QuakeC             WORKING   rounds, zombies, damage, game over
Menus              WORKING   main, configuration, map select, pre-game
HUD                PARTIAL   scoreboard/game over OK; in-game elements under review
SFX                WORKING   24 kHz mixer -> nzpsnd.irx -> SPU2 (audibility on HW untested)
Music              TODO      needs offline ADPCM conversion + SPU2 voice streaming
Fog                PARTIAL   GS per-vertex fog; interaction with lightmap pass to verify
Memory report      WORKING   startup / map load / spawn / unload / OOM
Profiler           WORKING   ps2_profile 1
Memory card        TODO      config currently written next to the data
Networking         TODO      loopback only; DEV9/ps2ip later
Real hardware      TODO
```

## Measured

* 30 fps locked (every other vblank) in menus and in nzp_warehouse / ndu.
* EE free after spawning on nzp_warehouse: 10.7 MiB (peak EE use ~20.5 MiB).
* VRAM: 1.6 MiB frame/depth, 2.4 MiB texture ring.

## Next

1. View model visibility and HUD elements in game.
2. Fog vs. lightmap pass ordering.
3. Config / settings on the memory card.
4. Music: offline ADPCM conversion + streaming.
5. Profile on real hardware; VU1 transform only where profiling shows it.
