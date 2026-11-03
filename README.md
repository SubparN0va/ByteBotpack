# ByteBotpack

<img src="rlbot/logo.png" alt="Byte" width="420">

Byte, Kilobyte and Megabyte for **RLBot v5**, with an optional shared model and
rank selector. Supports 1v1, 2v2 and 3v3.

## Play

Add `rlbot/bot.toml` from a built package to RLBot. Keep the complete bot folder
together. The default is full-strength Megabyte with the selector enabled.
Choose Byte, Kilobyte or Megabyte at full strength, or Select Skill for estimated
Bronze I through SSL difficulty. Auto tries to keep the score even.

All cars appear in one window, blue team first. Changes apply with **Apply &
respawn**. The window opens without taking focus from Rocket League. It uses
native Windows controls: no Qt, Python or browser installation is needed to play.

GPU inference is the default. Windows, a compatible NVIDIA GPU and driver, and
LibTorch **2.9.1+cu126** are required. The shared runtime belongs at
`%LOCALAPPDATA%/RLBot5/bots/libtorch`, or set `LIBTORCH_CUDA_ROOT` to its directory.
The runtime is not bundled and GPU builds do not silently fall back to CPU.

## Run without the selector

Edit this line in `rlbot/bot.toml`:

```toml
run_command = "GGLBot.exe --manual=Off" # Options: Off (GUI), Byte, Kilobyte, Megabyte (full strength, no GUI)
```

Choose a named model to disable the GUI and rank adjustments. Manual startup
updates the configured name without changing saved GUI preferences. RLBot may
need an entry refresh and a new match to display the corrected name.

Rank labels are provisional estimates, not verified human ranks.

## Build for the botpack

This repository contains runtime and build sources, the GUI, logo and three
inference model pairs. Training, benchmarking and development tests are not
included. Generated executables are ignored by Git; source checkouts must be built.

Build machines need bob, Docker with Linux containers, Python 3.10+, and the
complete Windows LibTorch SDK matching the runtime:

```text
python scripts/build_bob.py --bob "path/to/bob.exe"
```

The result is under `bob_build/ByteBotpack/` in this repository. By default, the
helper temporarily stages SDK headers and import libraries in
`build-support/libtorch/local/` and removes that generated copy when it finishes,
matching the upstream build helper. No particular drive letter is required.
Use `--work-dir "path/to/scratch"` for an isolated build snapshot, `--out-dir` to
change the output directory, or `--libtorch "path/to/libtorch"` to select an SDK.
A plain bob build without the helper's staged SDK is not supported. For a native build, use an x64 Visual Studio developer prompt with CMake and Ninja:

```text
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGLBOT_DEVICE=gpu -DGGLBOT_DEPLOY_DIR=out/package
cmake --build build/native --parallel 4
```

For a source-only archive:

```text
python scripts/package_bot.py
```

The six inference files are grouped under `rlbot/models/byte`, `kilobyte` and
`megabyte`. Their checksums are in the [model manifest](rlbot/models/manifest.json).

## License and credits

Original contributions are under the [MIT license](LICENSE), copyright 2026
SubparNova. Third-party code retains its original authorship and license terms:

| Component | License notice |
| --- | --- |
| RLBot C++ interface, Michael Theall | [MIT](cpp-interface/LICENSE) |
| RocketSim, ZealanL | [MIT](inc/RocketSim/LICENSE) |
| Bullet 3.24 | [zlib notice and exceptions](inc/RocketSim/libsrc/bullet3-3.24/LICENSE.txt) |
| FlatBuffers 25.2.10 | Apache 2.0 (upstream dependency) |

The runtime inherits GGLBot, GigaLearn and RLGym components. Their existing source
notices remain intact. No separate license for the inherited GigaLearn/RLGym
portions was present in the imported snapshot; the root MIT license does not
resolve that omission. Their redistribution terms still need confirmation.

RLBot's interface fetches FlatBuffers and the RLBot schema at the revisions in
`cpp-interface/CMakeLists.txt`; Linux builds also fetch liburing. These retain
their upstream terms. PyTorch/LibTorch and NVIDIA runtime components are external
shared dependencies and are not bundled.
