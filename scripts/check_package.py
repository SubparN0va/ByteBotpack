"""Validate a built ByteBotpack package, including output from bob split."""
from __future__ import annotations

import argparse
import tomllib
from pathlib import Path
import sys

from package_bot import validate_models


PLATFORMS = {
    "x86_64-pc-windows-msvc": "GGLBot.exe",
    "x86_64-unknown-linux-gnu": "GGLBot",
}


def check_package(package: Path) -> list[Path]:
    for name in ("bot.toml", "loadout.toml", "logo.png", "device.txt"):
        if not (package / name).is_file():
            raise ValueError(f"Missing package file: {package / name}")
    mode = (package / "device.txt").read_text(encoding="utf-8").strip()
    if mode not in ("cpu", "gpu"):
        raise ValueError("Packaged device.txt must contain cpu or gpu")

    launchers = [(package / platform, launcher) for platform, launcher in PLATFORMS.items()
                 if (package / platform).is_dir()]
    if not launchers:
        launchers = [(package, name) for name in PLATFORMS.values() if (package / name).is_file()]
    if not launchers:
        raise ValueError(f"No platform or flat launcher found in {package}")

    config = tomllib.loads((package / "bot.toml").read_text(encoding="utf-8"))
    agent_id = config.get("settings", {}).get("agent_id")
    profiles = {"SubparNova/KilobyteReduced": "kilobyte", "SubparNova/Megabyte": "megabyte"}
    if agent_id not in profiles:
        raise ValueError(f"Unknown bot agent_id: {agent_id}")
    profile = profiles[agent_id]
    for directory, launcher in launchers:
        if directory != package and (directory / "bot.toml").exists():
            raise ValueError(f"Platform configuration would shadow the shared bot.toml: {directory}")
        suffix = ".exe" if launcher.endswith(".exe") else ""
        core = "GGLBotCoreCUDA" if mode == "gpu" else "GGLBotCoreCPU"
        gui = (f"000-runtime/ByteSkillUI{suffix}",) if profile == "kilobyte" else ()
        for name in (launcher, f"000-runtime/{core}{suffix}", *gui):
            if not (directory / name).is_file():
                raise ValueError(f"Missing platform file: {directory / name}")
        # Check beside each launcher, never against a shared directory that
        # happens to exist in an unsplit developer build.
        validate_models(directory, (profile,), source_layout=False)
        expected = {"POLICY.lt", "SHARED_HEAD.lt"}
        actual = {path.relative_to(directory).as_posix()
                  for path in directory.rglob("*.lt")}
        if actual != expected:
            raise ValueError(f"Unexpected model files for {profile}: {directory}")
    return [directory for directory, _ in launchers]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Built ByteBotpack directory containing bot.toml")
    args = parser.parse_args(argv)
    try:
        for directory in check_package(args.package.resolve()):
            print(f"Verified launcher, core and required model files: {directory}")
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(f"Package validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
