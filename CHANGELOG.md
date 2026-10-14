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
- **Optional FSAA** (`FSAA=1`): renders twice as wide and filters down.
- Measurement tools: a frame profile (`GFX_PROFILE=1`), per-handler timers
  (`=2`) and a sampling profiler (`=3`, writes `build/dc/prof_samples.bin`);
  `AUTO_EXIT_SECONDS`, `DEMO_LEVEL`, `SKIP_WATER`, `TEX_TRACE`; and a debug view
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

### Fixed

- Sound effects were silently dropped while music and engine loops played,
  because the SH4 refused work it counted as over budget.

## Scenes

### Added

- **A scene gate.** It closes when a scene starts loading and opens once
  everything the scene needs is resident and its first frame is on screen.
  Until then no sound or song starts, and the load's time is not counted as
  game time.

## Build

- The disc build starts its audio folder afresh, so files from older builds do
  not stay on the disc.
