<p align="center"><img src="art/icon.png" alt="JSRF Unleashed for the PS5" width="320"></p>

# JSRF Unleashed for the PlayStation 5

A port of [JSRF Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed) (0.5.9, commit `414b103`) to
native PS5 homebrew. JSRF Unleashed is Jet Set Radio Future, statically recompiled from the Xbox executable into C.
On the PS5 it runs as an installed title (`PPSA69431`) and draws through a Vulkan renderer on
[Mihawk's RADV port](https://github.com/mihawk-99/PS5_Vulkan).

You need your own copy of the game. This repository holds no game data and no game code: the C code is generated
on your machine from your own `default.xbe`, and the title reads the rest from your disc image.

## Status

An early release. Tested on one console (etaHEN, ShadowMountPlus, the internal SSD):

- Installing from a disc image, the start menu, the game's menus and play with sound and the DualSense
- Busy stretches at 58 to 60 fps at render scale 2, output at 3840x2160
- The camera on the right stick, saves, and keeping the game and saves across an update of the title

## Installing

1. Download `JSRFUnleashed-PS5-<version>.zip` from the [releases](https://github.com/Marice/jsrf-ps5/releases) and
   unpack the `PPSA69431` folder to `/data/homebrew/` on the console (ShadowMountPlus installs it). ProsperoStore
   installs it for you.
2. Put your disc image of **Jet Set Radio Future (USA)**, a `.iso` file, in `/data/homebrew/PPSA69431/dump/`. Only
   the standalone US release runs.
3. Start **JSRF Unleashed**. The first start checks the image and unpacks it, which takes about ten minutes. It
   happens once. After that you can remove the disc image.

The title keeps the unpacked game, your saves and your settings in `/data/jsrf-ps5`, outside its own folder. An
update of the title, by hand or through ProsperoStore, replaces only the title folder, so the game does not unpack
again and your saves stay. A title that cannot write `/data` (on another drive) keeps them in its own folder.

| Path | What it holds |
| --- | --- |
| `/data/jsrf-ps5/game/` | The game's files, unpacked from your disc image |
| `/data/jsrf-ps5/save/UDATA/` | Your saves. Copy this folder to back them up. |
| `/data/jsrf-ps5/settings` | The start menu's choices |
| `/data/homebrew/PPSA69431/jsrf.log` | The log of the last start |

## The start menu

Before the game starts, a short menu counts down five seconds and starts with the last choices. Press X to change
them, up and down to choose a row, left and right to change it, and X on START GAME to start.

| Row | What it does |
| --- | --- |
| SCALE | The render resolution, 1x to 6x the Xbox's 640x480 (6x fills a 4K screen) |
| WIDESCREEN | A wider 16:9 view, not a stretch |
| CAMERA | RIGHT STICK turns the camera around the skater; ORIGINAL keeps the game's own camera |
| DEBUG MODE | Test modes on the touchpad, for development |

## Building

The build runs on Linux (tested on WSL2). It is not turnkey yet: it uses the RADV setup of
[rush2-ps5](https://github.com/Marice/rush2-ps5) from a checkout next to this one.

You need:

- A checkout of rush2-ps5 at `../rush2-ps5`, with Mihawk's RADV port in `radv/mihawk-vulkan-review` and its Mesa
  headers in `radv/PS5_Mesa`, built as described there, and its Docker build image `rush2-ps5-build`
- [glslang](https://github.com/KhronosGroup/glslang) 16.2.0: the source in `.deps/glslang`, built for the PS5 with
  the SDK's `prospero-clang18` into `build-vk/glslang` (Release; `ENABLE_HLSL`, `ENABLE_OPT`, `ENABLE_RTTI`,
  `ENABLE_EXCEPTIONS` and `ENABLE_GLSLANG_BINARIES` off)
- Python 3.10 or later for upstream's code generator, and CMake

Then:

1. `tools/setup-source.sh` checks out JSRF Unleashed at the pinned commit into `src-upstream/` and applies
   `patches/jsrf.patch`.
2. Generate the game's C code in `src-upstream/`, as upstream's README describes under building from source: put
   your `default.xbe` in `src-upstream/game_files/` and run its tools into `port/recomp/gen`.
3. `./build-vk.sh` builds the title into `pkg-vk/PPSA69431`. `tools/setup-pacbrew.sh` fetches the PacBrew ports
   (checked by hash) on the first run. A full build compiles over a million lines of generated C. On a machine with
   little memory, limit the parallel compiles, for example `JOBS=4 ./build-vk.sh`.

To change upstream's code, edit it in `src-upstream/` and save the change with `tools/save-patch.sh`. The patch is
the source of truth: `tools/setup-source.sh` refuses to overwrite changes that are not saved in it.

## Layout

| Path | What it holds |
| --- | --- |
| `ps5/` | The PS5 layer: start menu, disc import, status screen, POSIX gaps, the Vulkan build's CMake and link scripts |
| `patches/jsrf.patch` | The changes to JSRF Unleashed, including the Vulkan renderer `src/nv2a_vk/nv2a_vk.c` |
| `tools/` | Source and dependency setup |
| `sce_sys/`, `art/` | The title's parameters, icon and artwork |

Comments in the code refer to numbered decisions (D1, D2, ...). These are from the project's working notes, which
are not in this repository.

## Credits

- [JSRF Unleashed](https://github.com/JuMatt/JetSetRadioFuture-Unleashed) and its contributors, the recompilation
  this port builds on
- [Mihawk's RADV port](https://github.com/mihawk-99/PS5_Vulkan) for Vulkan on the PS5
- [ps5-payload-dev](https://github.com/ps5-payload-dev) for the payload SDK and the PacBrew ports
- [glslang](https://github.com/KhronosGroup/glslang) for the shader compiler

Jet Set Radio Future is a trademark of SEGA. This project is not affiliated with or endorsed by SEGA.

## License

MIT, see [LICENSE](LICENSE). JSRF Unleashed is MIT licensed too; the changes in `patches/jsrf.patch` fall under its
license.
