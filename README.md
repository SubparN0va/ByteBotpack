# ByteBotpack

<img src="rlbot/logo.png" alt="Byte" width="420">

Byte, Kilobyte and Megabyte for **RLBot v5**, with an optional rank selector. Supports 1v1, 2v2 and 3v3.

## Play

Add the `bot.toml` at the root of the built package to RLBot. Keep the complete
bot folder together. For a native build deployed into this checkout, use
`rlbot/bot.toml`. The default is full-strength Megabyte with the selector enabled.
Choose Byte, Kilobyte or Megabyte at full strength, or Select Skill for estimated
Bronze I through SSL difficulty. Auto tries to keep the score even. Changes apply with **Apply &
respawn**.

CPU inference is the default on Windows and Linux. It uses the CPU runtime already
installed with the official RLBot botpack. On Windows this is normally
`%LOCALAPPDATA%/RLBot5/bots/torch-archive/torch`; set `LIBTORCH_CPU_ROOT` to that
`torch` directory if it is installed elsewhere. Byte does not download another
runtime when it starts. The selector window is available on Windows.

GPU inference requires a separate Windows build and a compatible CUDA LibTorch
installation; see the GPU build instructions below.

## Run without the selector

By default, the selector window opens when the bot starts. To disable it, append
`--manual=Byte`, `--manual=Kilobyte`, or `--manual=Megabyte` to the existing
`run_command` in the package-root `bot.toml`. Preserve the executable path that
Bob generated. For example, a Windows Bob package can use:

```toml
run_command = 'x86_64-pc-windows-msvc\GGLBot.exe --manual=Megabyte'
```

Each named model runs at full strength without the selector. Replace the argument
with `--manual=Off`, or remove it, to enable the selector again. For Linux CPU
packages, append the same argument to the existing `run_command_linux`.

Rank labels are provisional estimates, not verified human ranks.

## Build for the botpack

From a source checkout with Bob and Docker (Linux containers) installed, run:

```text
bob build bob.toml --out-dir "path/to/build-output"
```

The package is written to `path/to/build-output/ByteBotpack/`. Without `--out-dir`,
Bob uses `bob_build/ByteBotpack/` in this repository.

The checked-in `rlbot/device.txt` selects `cpu`. A fresh CPU build automatically
downloads the Windows and Linux SDKs from the public
[RLBot CPU PyTorch archive](https://github.com/VirxEC/pytorch-archive/releases/tag/r-1).
Both downloads are pinned to that release and checked against SHA-256 hashes.
No local LibTorch SDK or Python installation is needed for a plain Bob CPU build.
These are build-time downloads; the finished bot uses the shared runtime already
installed by RLBot.

The optional Python 3.10+ helper can create an isolated source snapshot:

```text
python scripts/build_bob.py --bob "path/to/bob.exe" --work-dir "path/to/scratch" --out-dir "path/to/build-output"
```

Its default CPU build also uses the public SDKs. To supply a different complete
Windows CPU SDK, pass `--libtorch "path/to/torch"` or set `LIBTORCH_CPU_ROOT`.
Use a version matching the runtime on the computer that will run the bot. An
explicit invalid SDK produces an error instead of downloading a replacement.

## Optional GPU build (Windows)

1. In a source checkout, change `rlbot/device.txt` from `cpu` to `gpu`.
2. Supply a complete release Windows CUDA LibTorch SDK and build with the helper:

```text
python scripts/build_bob.py --bob "path/to/bob.exe" --libtorch "path/to/libtorch" --work-dir "path/to/scratch" --out-dir "path/to/gpu-build-output"
```

The helper stages only the SDK headers and import libraries, fingerprints them
for Bob's cache, and removes its generated SDK copy after the build. The source
snapshot remains in the requested scratch directory. Without `--work-dir`, SDK
staging happens temporarily in `build-support/libtorch/local/` in the checkout.

To run the GPU package, use a compatible NVIDIA GPU and driver, and the same
CUDA LibTorch version used for the build. Set `LIBTORCH_CUDA_ROOT` to its
`libtorch` directory, or install it at `%LOCALAPPDATA%/RLBot5/bots/libtorch`.
The helper also checks that location when `--libtorch` is omitted. CUDA runtime
DLLs are supplied by the user and are not included in the package.

Device selection happens before compilation. Changing `device.txt` in an already
built package does not switch CPU/GPU; rebuild from source. A GPU build does not
fall back to CPU, and plain Bob GPU builds require a staged CUDA SDK. Keep `cpu`
in the source configuration submitted to the botpack.

The six inference files are grouped under `rlbot/models/byte`, `kilobyte` and
`megabyte` in the source checkout, or `models/` in a built package. Their checksums
are in the [model manifest](rlbot/models/manifest.json).