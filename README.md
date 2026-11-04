# ByteBotpack

<img src="rlbot/logo.png" alt="Byte" width="420">

Megabyte and Kilobyte for **RLBot v5**. Supports 1v1, 2v2 and 3v3.

## Play

Add the two package-root `bot.toml` files to RLBot. Keep each complete bot folder
together:

- **Megabyte** runs at full strength without opening a GUI.
- **Kilobyte** allows for selecting a custom difficulty. Once the bot is started, the rank selector opens.
By default, it plays at a Champ I level. Choose Auto or Bronze I through SSL, then
**Apply & respawn** for the changes to take effect.
  Reduced ranks display as `Kilobyte[Champ I]`, for example. SSL displays simply
  as **Kilobyte**, without a rank suffix. Auto tries to keep the score even.

Fresh packages retain the `Kilobyte[Champ I]` name until a rank is explicitly
saved/applied. Rank labels are provisional
estimates, not verified human ranks. Each Kilobyte car can have its own rank;
Megabyte cars are excluded from the selector.

CPU inference is the default on Windows and Linux. It uses the CPU runtime already
installed with the official RLBot botpack. On Windows this is normally
`%LOCALAPPDATA%/RLBot5/bots/torch-archive/torch`; set `LIBTORCH_CPU_ROOT` to that
`torch` directory if it is installed elsewhere. The bots do not download another
runtime when they start.

GPU inference requires a separate Windows build and a compatible CUDA LibTorch
installation; see the GPU build instructions below.

## Startup options

Each executable's profile is fixed at build time with `GGLBOT_PROFILE` (`kilobyte`
or `megabyte`), independently of the display name or Bob's generated launch
command. Bob selects this automatically for each bot. There is no model selector
or `--manual` option.

Kilobyte supports `--skill=0` through `--skill=22`, `--no-ui`, `--debug` and
`--ui-only` (save a default before a match). `GGLBOT_SKILL` supplies a
rank when `--skill` is absent; saved preferences are the final fallback. Append
options to the existing Bob-generated executable path in `run_command` or
`run_command_linux`. Use `--controller` to reopen the live Linux rank window after
closing it; closing the window leaves the playing bot running.
Megabyte ignores rank preferences and environment overrides, and rejects rank/GUI
arguments. Both profiles support `--check-models` for an offline inference check.

## Linux selector

The Linux controller uses [GTK 3](https://docs.gtk.org/gtk3/) in a desktop session
(X11 or Wayland). Install your distribution's GTK 3 runtime if it is missing.
On Ubuntu 24.04 this is `sudo apt install libgtk-3-0t64`; older Debian/Ubuntu
releases call the package `libgtk-3-0`.

Start Kilobyte[Champ I] normally in RLBot to open the live rank selector. It has
the same per-car pending ranks, Auto, Apply & respawn, and saved defaults
as Windows. The controller runs separately from inference, so applying ranks can
restart bots without closing the panel. Only one controller opens per package
and RLBot server.

`GGLBot --ui-only` opens the default selector without loading Torch. On a headless
machine, use `--no-ui` with a saved rank or `--skill=N`. The Linux GUI executable
is `000-runtime/ByteSkillUI` beside the platform launcher; Bob split preserves it.
GTK is linked only into that executable; Megabyte and headless inference do not
require GTK. Native Linux builds need the development package `libgtk-3-dev` and
`pkg-config`; the Bob Dockerfile installs them automatically.

## Build for the botpack

From a source checkout with Bob and Docker (Linux containers) installed, run:

```text
bob build bob.toml --out-dir "path/to/build-output"
```

Bob writes `KilobyteReduced/bot.toml` and `Megabyte/bot.toml` under the output
directory. KilobyteReduced is the first/default project in `bob.toml`. Without
`--out-dir`, Bob uses `bob_build/`. Each project has a separate configuration,
fixed profile and model pair; Bob can safely rewrite both launch commands.

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

## Native build and validation

For a native CMake build, set `GGLBOT_PROFILE=kilobyte` (the default) or
`GGLBOT_PROFILE=megabyte`, and optionally `GGLBOT_DEPLOY_DIR`. Use separate output
directories for the two profiles. The default deploy directories are
`rlbot/kilobyte/` and `rlbot/megabyte/`; add their `bot.toml` files after building.

Each source bot folder (`rlbot/kilobyte/` and `rlbot/megabyte/`) contains its
`bot.toml`, `POLICY.lt`, and `SHARED_HEAD.lt`. Shared assets live directly under
`rlbot/`. There is no Byte policy.
Each built package contains only its own policy pair, beside the launcher:
inside each platform folder for Bob builds, or at the package root for flat
native builds. This location survives `bob split`, which drops shared root
directories while retaining platform folders and individual root files.

Validate both packages for each platform after splitting:

```text
bob split "path/to/build-output"
python scripts/check_package.py "path/to/build-output_x86_64-windows/KilobyteReduced"
python scripts/check_package.py "path/to/build-output_x86_64-windows/Megabyte"
python scripts/check_package.py "path/to/build-output_x86_64-linux/KilobyteReduced"
python scripts/check_package.py "path/to/build-output_x86_64-linux/Megabyte"
```

Run each platform launcher with `--check-models` to load its policy and perform
an inference check without connecting to RLBot or changing saved selections.
Configure with `-DGGLBOT_BUILD_TESTS=ON` to build the `skill_profiles` and
`package_paths` regression tests, plus `controller_linux` on Linux. CTest checks
ranks, names, mixed rosters, persistence, the native selector, and Linux controller
detachment and singleton behavior. On headless Linux, run
`xvfb-run -a ctest --test-dir <build> --output-on-failure` (requires Xvfb and xauth). Python package tests run with
`python -m unittest discover -s tests -v`; set `BYTE_PACKAGE_TEST_ROOT` to a
scratch directory, and `BOB_EXE` / `PACKAGE_PROBE` to enable the real Bob splitter
and compiled path checks.