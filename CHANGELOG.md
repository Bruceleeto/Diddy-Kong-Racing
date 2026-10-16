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

- **Frame-rate readout** in the top-right corner, drawn with enDjinn's quick
  font, instead of the yellow debug text on the left.
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
- The sampling profiler can sample any one map (`PROF_SAMPLE_MAP`), and
  `GFX_PROFILE=4` counts only the samples of frames that were late.
- The scene report gives the PVR's render time per scene (`SCENE PVR`: map,
  average, longest, frames over one field). `FILL_PROF=1` adds what each list
  fills, by display-list category and translucent class, in screens of area
  and tiles, the surfaces that fill most, and one `FF` line per frame for
  fitting render time to fill. `PROF_SKIP` leaves part of the frame out
  (a list, a category, a translucent class); `PROF_SKIP=999` cycles through
  them every ten frames to measure them all in one run.
- **The profile no longer makes the frames it measures late.** Its lines, the
  game's debug messages and the `fps:` line went out with printf as they came,
  each a round trip to the host over dcload, and a `GFX LONG FRAME` line made
  the frames after it long too: two runs of the same build gave race 28 10 and
  39 late frames. While a scene plays they now collect in RAM and print while
  the next scene loads; two runs now agree. The `fps:` line is only in profile
  builds. The scene report also lists the combiners that ran per vertex
  (`SCENE SLOWCC`) and how often batches were classified (`SCENE CC`).
- The scene report gives the display-list walk's cache stalls a frame,
  operand and instruction cache apart (the SH4's second performance counter),
  texture lookups and scans a frame, and where the walk's stack is.

### Changed

- Opaque geometry goes to the PVR's opaque list, alpha-tested geometry to the
  punch-through list, and only blended geometry to the translucent list.
  Blending is decided from the N64 blender equation, not just `FORCE_BL`.
- Every triangle goes through the fast path. Racer models with matrix selects
  around their vertex loads, sprites, alpha-tested, depth-off and decal
  batches, and combiners that mix shade all used to fall back to the emulated
  path.
- **Frames are paced on the vblank interrupt** (`VBL_PACER=1`, the default)
  instead of a sleep to a clock time, which overshot its target by up to 5 ms.
  Late frames in a 5-minute attract run fell from 103-174 to 60-61, most of it
  in the character demo (62-63 to 23-24 on its first visit). Game speed follows
  the real refresh rate (59.94 Hz on NTSC, as on the N64); every attract scene
  lasted the same number of frames as before.
- **Level geometry is culled per batch** (`LEVEL_CULL=1`, the default). The
  game only drops whole segments; inside a visible segment it drew every batch,
  and in the attract races 16-35% of those triangles lie outside the view. A
  batch whose bounding box is wholly outside one side of the view is now
  skipped. Level display-list time in races fell by 0.25-0.6 ms a frame and
  late frames in races 18 and 28 from 9 and 16 to 1 and 9 per 5-minute attract
  run. The GFX_PROFILE build checks every culled batch against its own
  vertices and the level's draw matrix against the cull matrix.
- **The circle transition's solid black is opaque.** It was all blended, two
  full-screen fills with FSAA. Its solid quarters are now a cover in the opaque
  list, nearer than the whole scene and writing depth, so the punch-through and
  translucent lists drawn after it stay under it; only the soft edge is
  blended. A cover falls back to blending when 2D has already gone to the
  translucent list that frame, which would show through it. With FSAA the
  title beach's scene had 76-83 late frames, all in the iris-out at its end; it
  now has none there.
- **Fades to black are drawn into the frame.** A full-screen fade was a
  blended rect over every pixel, about 4.5 ms of PVR time a frame with FSAA.
  The fade announces itself while the display list is built, and the port
  draws everything before it with its colours scaled by what the fade leaves
  of them, which is what blending black over them does, and leaves the rect
  out. Each attract race opens with a 22-frame fade-in; with FSAA every one of
  those frames was late. Frames the PVR alone made late fell from 35 to 14 in
  race 18 and from 21 to 1 in race 28, and late frames in race 18 from 35 to
  12-14 (two runs each, 120 s attract benchmark). The logo, swirl and title
  scenes render about 1 ms faster on average. Fades to white still blend.
- Per-batch render state is cached: combiner classification, compiled polygon
  headers, texture lookups and scissor checks.
- PVR render time fell by about a third with twiddled textures.
- **Display-list translation is 12-17% cheaper.** About half of it was the
  CPU waiting on its caches, both direct-mapped (8 KB of code, 16 KB of
  data). The handlers every batch runs now sit in `.sub0`, which KOS aligns to
  the instruction cache and checks fits in it, with their cold paths split
  out, and the walk's state in one data section aligned to the operand cache;
  so they no longer evict each other, and an unrelated change no longer moves
  them. The blast path projects each vertex once all the way to what the PVR
  is sent, two vertices at a time, and writes vertices with registers-only
  pointers, pre-decrement stores and (for the recorded lists) `movca.l`; it
  prefetches the batch's vertices, triangles and the display list. Combiners
  that mix shade are compiled per state rather than interpreted per vertex,
  and a texture is looked up only when the texture state changes. In the
  120-second attract run (each build also linked 384 bytes higher, to show
  what is layout), display-list time and late frames went from 6.95-7.08 ms
  and 27-29 to 5.66-5.78 ms and 8 in the title scene, 7.95-8.20 ms and 0-2 to
  6.79-7.00 ms and 0-1 in race 18, and 10.94-11.57 ms and 9-10 to 9.80-9.94 ms
  and 5 in race 28. HDMI captures match the old build's frames to within
  rounding (at most 2 levels, in about 5% of pixels).

### Fixed

- The intro's cutscene camera flickered with the player camera at 60 fps.
- A level's load time was paid back afterwards as game time, so the first
  moments of every race ran up to six times too fast.
- Racers no longer accelerate and boost harder at 60 fps.
- Memory: three oversized buffers left the profile build without room for
  its assets.

### Tried and removed

- Triangle strips: 36% fewer vertices, but the display list got 2–4 ms slower.
- Prefetching the display list four commands ahead, and keeping the blast
  path's clip-space corners only for batches that need the near-clipper: each
  made display-list time 0.15-0.3 ms a frame worse in every attract scene.
  That was measured while the hot code still moved with every change, which
  alone shifts display-list time by 2-6%; the display list is now prefetched.
- Drawing blended cutouts twice (punch-through under blended): no better edges,
  up to 1 ms slower.
- Queuing every batch as its own enDjinn renderer (all three lists recorded,
  the display-list walk before the PVR wait, 170–310 renderers a frame in
  races): 168 and 169 late frames in two attract runs, against 58 and 56 for
  `ENJ_LOOP=1` with three renderers. Recording the opaque list and replaying it
  cost 0.6–0.8 ms a frame, up to 3.7 ms, on top of a 10% slower walk.

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
- AICAflow is pinned to its event-driven driver (firmware ABI 8,
  [AICAflow #12](https://github.com/dfchil/AICAflow/pull/12)): the AICA reports
  every change of state by interrupt, AICAflow's event thread frees an instance
  that ended, and its end callback queues the report for DKR, which completes
  the sound's owner or clears the song under its audio lock on the next tick.
  The callback only queues because the game holds that lock while it waits for
  instances to end. Each audio tick now only sends commands: no `afx_update()`,
  no recycling. In attract runs DKR's audio took 2.20 ms of SH4 time a second
  against 2.55 ms with the polling driver (event thread and callback
  included), and the music and effects played as before.
- No audio loader thread: a sound that no pack lists is loaded by the game
  thread between frames, outside any lock, instead of on a `sfx-loader`
  thread.

### Fixed

- Sound effects were silently dropped while music and engine loops played,
  because the SH4 refused work it counted as over budget.

## Boot screen

- **The Dreamcast swirl spins in place of the N64 logo.** It is a 3D model in
  the game's own object format, built by `dreamcast/swirl/make_swirl.py` from
  the swirl in the public-domain Dreamcast logo SVG on Wikimedia Commons, and
  embedded in the program; the port serves it when the boot screen loads the
  N64 logo model, so `assets.bin` is unchanged. It is orange, and blue on a
  European console, as the flash ROM's region says.
- **The swirl is lit and drawn natively.** Its faces are flat and its edge is
  rounded by a narrow bevel. Every frame each of its 1370 distinct vertices is
  lit on the SH4 with sh4zam from the rendering equation: Lambert diffuse and
  Cook-Torrance specular (GGX, Smith-Schlick, Schlick's Fresnel) for a
  lacquer, a warm key and a cool rim light that keep to the camera, the sky's
  irradiance, and the sky reflected, staying put as the swirl turns; then ACES
  tone mapped. The game's model is only a stand-in: the display-list walk
  projects each distinct vertex once and streams 184 triangle strips into the
  opaque list, instead of the game drawing 1828 triangles through 115 N64
  batches, which had cut the boot screen to 40 fps. It holds 60.
- **"Nintendon't".** On the title screen's beach, "n't" is scratched into the
  sand right after the Nintendo logo, at its letter size, as if drawn with a
  finger. It is the game's one bump-mapped surface: `dreamcast/scrawl/make_scrawl.py`
  finds the logo on the level model, draws the strokes as a height field and
  writes its normal map, which pvrtex makes a 512x512 VQ-compressed PVR bump
  texture (66 KB) over just the letters, without mipmaps so it stays sharp at
  the beach's low angle, and a grid of vertices that follows the sand. The level draw marks
  the spot after the logo's batch; the display-list walk draws it there in two
  passes, darkening the walls that face away from the light (to no less than
  60%) and lightening those that face it, so the sand around it stays exactly
  as it was.

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
- **Sprites and particle textures stay loaded for the scene.** Each time the
  last particle or sprite of a kind went, the game freed its textures, and the
  next spawn loaded and unpacked them again: in late frames of the title
  scene, a quarter of the CPU time was spent in gzip, the memory pool and
  `load_texture`. One whose last user lets go while the scene plays is now held
  with no users and found again in the cache; the next scene's load frees what
  is left, as does an allocation that needs the room. In the 120-second attract
  run late frames fell from 46 to 37, and no frame is more than one field late
  (the title scene had frames four fields long).

## Build

- **enDjinn is the Dreamcast build system.** `Makefile.dc` keeps DKR's options
  and asset pipeline and includes enDjinn's `base_link.mk`, which compiles
  every `.c` in `dreamcast/`, the game's sources and enDjinn's runtime, links
  and makes the disc image; `local.cfg.mk` holds the project settings. The
  compile flags are unchanged. The ELF and disc image are now
  `bin/dkracing.elf` and `bin/dkracing.cdi`.
- `ENJ_RENDER=1` submits each frame through enDjinn's render lists
  (`enj_state_startup`, `enj_render_scene`) instead of the port's own PVR scene
  calls: the display-list walk runs in the opaque list's renderer, and the
  recorded translucent and punch-through lists are replayed by theirs. It is
  off by default. In attract runs it matched the default within noise for late
  frames, but display-list time was 4% higher in every category: the same
  code, laid out about 600 bytes further on, so the hot loops (larger than the
  8 KB instruction cache) evict each other differently.
- `ENJ_LOOP=1` (with `ENJ_RENDER=1`) runs the game inside enDjinn's loop:
  `enj_state_run` calls one iteration of the game's loop as a mode updater,
  and the display list that iteration built is submitted when it returns,
  which is the same moment the plain loop draws it at the top of the next
  iteration. Off by default. In two interleaved attract runs each way it had
  54 late frames against 58, every scene lasted the same number of frames, and
  the pictures matched.
- LTO uses one partition (`-flto-partition=one`). With the default
  partitioning, enDjinn's object order changed how the display-list code was
  inlined and laid out: 1.5x the frames with over 11 ms of display-list time.
  With one partition there were about a third as many as before the switch,
  and the result no longer depends on the link order.
- **`assets.bin` stays on disc.** The port loaded the whole 10 MB asset
  image into RAM, as a cart is to the N64; each asset "DMA" now reads it from
  the file, which leaves 8.8 MB free. What scenes read comes from RAM:
  - every section under 128 KB (tables, headers, text) and the 2D textures
    (HUD, menu and sprite images), 1.3 MB, are read at boot and kept;
  - each scene has a pack (`build/dc/assetscenes/LEVEL.dsp`, made by
    `dreamcast/build_asset_scenes.py`), which the scene gate reads in one pass
    when the scene starts loading and drops when the next one does. It holds
    the level's header, level model, object map and level-model textures,
    found from the level itself, and what `dreamcast/assets.scenes` lists:
    ranges a `TEX_TRACE=1` run saw the scene read beyond that (the objects it
    spawns), merged by `dreamcast/record_scenes.py`. The attract scenes' packs
    are 12–414 KB, read in 4–56 ms over `/pc`.

  In two attract runs nothing was read from disc while a scene played, scene
  loads took as long as with the whole image in RAM (2.0–2.4 s for a race),
  and there were 50 and 46 late frames (53 and 55 before). The N64 sound
  samples (3.8 MB) are never read, since AICAflow plays its own. Reads while a
  scene plays are counted per scene (`ASSET PLAY` with `GFX_PROFILE=1`).
  `ASSET_RESIDENT=1` loads the whole image as before; `ASSET_TRACE=1` prints
  which parts of it a run read.
- `ENJ_INPUT=1` reads all four controller ports through enDjinn's controller
  layer (`enj_ctrl`) instead of port 0 through KOS maple calls, and reports
  every connected pad to the game, so two to four players can join. Off by
  default until it has been played with.
- `dreamcast/build.sh` builds the port from a clean clone and your ROM, after
  checking the toolchain, `pvrtex`, sh4zam and Python. A fresh clone built with
  it produces the same textures, scene bundles and AICAflow files, byte for
  byte, as the development tree.
- `Makefile.dc` fetches AICAforge, which AICAflow pins with `update = none`,
  so a recursive clone no longer stops at the missing sound tools.
- The disc build starts its audio folder afresh, so files from older builds do
  not stay on the disc.
