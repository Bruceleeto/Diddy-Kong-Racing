# Changelog

Changes in this fork ([dfchil/Diddy-Kong-Racing](https://github.com/dfchil/Diddy-Kong-Racing))
since it diverged from
[Bruceleeto/Diddy-Kong-Racing](https://github.com/Bruceleeto/Diddy-Kong-Racing)
at `7ec9edf5` (that repository's merge of the first AICAflow integration).
All of it concerns the Dreamcast port.

Measured on a Dreamcast with a broadband adapter, in the attract-mode demos
(`AUTO_EXIT_SECONDS`, `GFX_PROFILE=1`):

| | `7ec9edf5` | Now |
|---|---|---|
| Frame rate in races | 30, locked | 59.8–60.1 |
| PVR render time in races | 10.0–10.9 ms | 6.6–6.7 ms |
| Worst frame in races | 35–100 ms | 25–26 ms |
| Sound effects that never played | several per race | none seen |
| Textures decoded on the console | all, on first use | none |

## Graphics

### Added

- **60 fps** (`FRAME_FIELDS=1`). The game logic keeps the two-field step its
  physics is tuned for; a frame that steps it draws the scene part-way between
  the old state and the new one, and the next frame draws the new state. Boosts,
  acceleration, timers and demos behave exactly as at 30 fps.
- **24-bit framebuffer** (RGB888) instead of RGB565, so skies, fog and fades no
  longer band or dither.
- **Offline textures.** `dreamcast/build_pvr_textures.py` converts every texture
  asset at build time from the extracted PNGs with `pvrtex`, into
  `build/dc/textures.dtp`. Each texture gets the format its alpha needs (RGB565,
  ARGB1555 or ARGB4444), invisible texels take the colour of their visible
  neighbours (no dark rims on plants and sprites), and all textures are
  twiddled. A texture asset's frames go into video memory while its scene
  loads, through enDjinn's loader; nothing is decoded while a scene runs.
  `TEXTURE_PVRTEX_FLAGS` passes options such as VQ (`-c`) or mipmaps (`-m`).
- **enDjinn** as a submodule (`third_party/enDjinn`), used for its texture
  loader.
- **Optional FSAA** (`FSAA=1`): renders twice as wide and filters down. With
  60 fps it holds 58.9–59.5 fps in races and 56.8 in the character demo, and
  leaves about 0.55 MB of video memory free; without FSAA it is 1.5 MB.
  With FSAA, translucent particles (smoke, dust) are thinned to every other
  one, each drawn with the coverage of two, and sprites that fill much of the
  screen fade out (`FSAA_PARTICLE_THIN`, `FSAA_PARTICLE_NEAR`). In a 5-minute
  attract run that cut late frames from 417 to 306 over the same seven scenes;
  without FSAA there are 118.
- Measurement tools: a frame profile (`GFX_PROFILE=1`), per-handler timers
  (`=2`) and a sampling profiler (`=3`, writes `build/dc/prof_samples.bin`);
  `AUTO_EXIT_SECONDS`, `DEMO_LEVEL`, `SKIP_WATER`, `SKIP_PARTICLES`, `TEX_TRACE`; and a debug view
  (`DEBUG_VIEW=1`) that holds a frame and switches cutout modes from the
  controller.

### Changed

- Opaque geometry goes to the PVR's opaque list, alpha-tested geometry to the
  punch-through list, and only blended geometry to the translucent list.
  Blending is decided from the N64 blender equation, not just `FORCE_BL`.
- Every triangle goes through the fast path. Racer models with matrix selects
  around their vertex loads, sprites, alpha-tested, depth-off and decal
  batches, and combiners that mix shade all used to fall back to the emulated
  path.
- Per-batch render state is cached: combiner classification, compiled polygon
  headers, texture lookups and scissor checks.
- PVR render time fell by about a third with twiddled textures.

### Fixed

- The intro's cutscene camera flickered with the player camera at 60 fps.
- A level's load time was paid back afterwards as game time, so the first
  moments of every race ran up to six times too fast.
- Racers no longer accelerate and boost harder at 60 fps.
- Memory: three oversized buffers left the profile build without room for
  its assets.

### Tried and removed

- Triangle strips: 36% fewer vertices, but the display list got 2–4 ms slower.
- Drawing blended cutouts twice (punch-through under blended): no better edges,
  up to 1 ms slower.

## Audio (AICAflow)

### Added

- **A per-scene AICA RAM plan.** `dreamcast/build_aicaflow_budget.py` charges
  every scene its vehicle pack, scene pack and the largest song, and fails the
  build if one does not fit.
- **Racer voice packs**, one per character and stored as ADPCM: 369 KB for all
  nine instead of 755 KB. The voices of the characters in a race load with the
  race, players first, as far as the plan leaves room.
- An attract-mode audio benchmark (`AICAFLOW_BENCHMARK=1`).

### Changed

- A level's vehicle pack, scene pack and song load before the level starts;
  each song loads its own samples and short jingles stay resident.
- The "Diddy Kong Racing!" and "Press Start" announcements, the fly-by and the
  incoming-rocket sound are resident; three sounds the later demos play are in
  their scene packs.
- AICAflow and AICAforge are pinned to their current mains, with the review
  and firmware fixes.

- No audio loader thread: a sound that no pack lists is loaded by the game
  thread between frames, outside any lock, instead of on a `sfx-loader`
  thread.

### Fixed

- Sound effects were silently dropped while music and engine loops played,
  because the SH4 refused work it counted as over budget.

## Scenes

### Added

- **A scene gate.** It closes when a scene starts loading and opens once
  everything the scene needs is resident and its first frame is on screen.
  Until then no sound or song starts, and the load's time is not counted as
  game time.
- **Scene texture bundles** (`build/dc/texscenes/LEVEL.dtb`). Each holds the
  textures a scene uses, from its level model's texture tables and from
  `dreamcast/textures.scenes`, and loads in one sequential read when the scene
  starts. The bundle keeps its textures for the whole scene, so models the
  character demo swaps in and out load without reading the disc. In the attract
  demos no texture is read while a scene runs; before, the character demo read
  up to 125 in one frame (up to 0.5 s).
  `dreamcast/record_scenes.py` merges the `TEXUSE` lines of a
  `TEX_TRACE=1` run into `textures.scenes`.
- **The gate also holds a scene's models and its cutscene song.** Object
  models a scene loaded while it ran (`dreamcast/models.scenes`, recorded like
  the textures) are loaded by the gate and kept until the next scene, so the
  character demo's racer swaps no longer read and unpack models from disc
  (50–330 ms each before). The song a cutscene starts a few frames in
  (`dreamcast/songs.scenes`) is made resident before the scene starts, and the
  scene's first logic step, which spawns its objects, counts as part of the
  load. In the attract demos no texture, model or sound is now read while a
  scene runs.

## Build

- The disc build starts its audio folder afresh, so files from older builds do
  not stay on the disc.
