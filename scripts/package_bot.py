"""Create a source submission zip without building the bot or installing LibTorch."""
from __future__ import annotations

import argparse
import os
import re
import posixpath
from pathlib import Path
import sys
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from build_bob import read_device


MODEL_PATHS = tuple(f"rlbot/{model}/{name}.lt"
                    for model in ("kilobyte", "megabyte")
                    for name in ("POLICY", "SHARED_HEAD"))
PACKAGE_PATHS = (
    ".dockerignore", ".gitattributes", ".gitignore", "README.md", "LICENSE",
    "bob.toml",
    "CMakeLists.txt", "cpp.Dockerfile",
    "build-support/libtorch/README.md", "cmake", "cpp-interface", "inc",
    "gui", "launcher", "scripts", "src", "tests",
    "rlbot/kilobyte/bot.toml", "rlbot/megabyte/bot.toml", "rlbot/loadout.toml", "rlbot/logo.png", "rlbot/device.txt",
    *MODEL_PATHS,
)
EXCLUDED_DIRECTORIES = {
    ".git", ".vs", ".vscode", ".idea", ".venv", "venv", "__pycache__",
    "build", "out", "bob_build", "CMakeFiles", "_deps", "Testing",
    "000-runtime", "libtorch", "torch-archive",
}
EXCLUDED_FILES = {
    "CMakeCache.txt", "CMakeSettings.json", "CMakeUserPresets.json",
    "CMakeLists.txt.user", "build.ninja", ".ninja_deps", ".ninja_log",
    "cmake_install.cmake", "install_manifest.txt", "compile_commands.json",
    "CTestTestfile.cmake", "skill-level.txt", "skill-choice.txt", "skill-roster.txt",
}
COMPILED_SUFFIXES = {".exe", ".dll", ".lib", ".obj", ".o", ".a", ".so", ".pdb", ".ilk", ".pyc", ".pyd"}


def submission_files(project: Path, output: Path) -> list[Path]:
    for name in PACKAGE_PATHS:
        if not (project / name).exists():
            raise ValueError(f"Missing submission input: {name}")
    for name in ("cpp-interface/library/CMakeLists.txt", "inc/RocketSim/src/RocketSim.h"):
        if not (project / name).is_file():
            raise ValueError("Missing submodule sources. Run git submodule update --init --recursive and retry.")
    read_device(project)
    validate_models(project / "rlbot", source_layout=True)

    def check_local(path: Path):
        if path.is_symlink() or path.resolve() != path:
            raise ValueError(f"Copy linked files into the project before packaging: {path.relative_to(project)}")

    def walk_error(error):
        raise error

    candidates = []
    for name in PACKAGE_PATHS:
        root = project / name
        check_local(root)
        if root.is_file():
            candidates.append(root)
            continue
        for directory, dirs, files in os.walk(root, onerror=walk_error):
            dirs[:] = [name for name in dirs if name not in EXCLUDED_DIRECTORIES and not name.startswith("cmake-build-")]
            if Path(directory) == project / "rlbot":
                # Local deployment copies documentation here; package its source only once.
                dirs[:] = [name for name in dirs if name not in ("docs", "licenses")]
            for name in dirs:
                check_local(Path(directory) / name)
            candidates.extend(Path(directory) / name for name in files)

    included = []
    for path in sorted(candidates):
        if path.relative_to(project).as_posix() in ("rlbot/README.md", "rlbot/LICENSE", "rlbot/THIRD_PARTY_NOTICES.md"):
            continue
        if (path.name == ".git" or path.name in EXCLUDED_FILES
                or path.suffix.lower() in COMPILED_SUFFIXES or ".so." in path.name):
            continue
        check_local(path)
        if path.resolve() == output:
            continue
        # Linux executables have no required extension; keep them out of source zips too.
        with path.open("rb") as stream:
            if stream.read(4) == b"\x7fELF":
                continue
        included.append(path)
    if not any(path.is_relative_to(project / "rlbot") and path.suffix == ".lt" for path in included):
        raise ValueError("No .lt models found in rlbot/. Add your trained models before packaging.")
    check_include_case(project, included)
    return included


def check_include_case(project: Path, files: list[Path]):
    # Windows accepts misspelled capitalization; bob's Linux filesystem does not.
    names = {path.relative_to(project).as_posix().lower(): path.relative_to(project).as_posix() for path in files}
    for path in files:
        if path.suffix not in (".h", ".hpp", ".cpp", ".cc"): continue
        relative = path.relative_to(project).as_posix()
        if relative.startswith(("inc/RocketSim/", "cpp-interface/")): continue
        roots = (posixpath.dirname(relative), "", "inc", "src", "cpp-interface/library/include")
        for include in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', path.read_text(encoding="utf-8", errors="replace"), re.M):
            for root in roots:
                candidate = posixpath.normpath(posixpath.join(root, include))
                actual = names.get(candidate.lower())
                if actual:
                    if actual != candidate:
                        raise ValueError(f"Include capitalization in {relative}: {include}; expected {actual}")
                    break


def validate_models(root: Path, profiles=("kilobyte", "megabyte"), *, source_layout=True):
    expected = {(f"{model}/" if source_layout else "") + f"{name}.lt" for model in profiles
                for name in ("POLICY", "SHARED_HEAD")}
    for relative in sorted(expected):
        path = root / relative
        if not path.is_file():
            raise ValueError(f"Missing model: {relative}")
        if path.stat().st_size == 0:
            raise ValueError(f"Empty model: {relative}")


def submission_content(project: Path, path: Path) -> bytes:
    data = path.read_bytes()
    if path.relative_to(project).as_posix() in ("rlbot/kilobyte/bot.toml", "rlbot/megabyte/bot.toml"):
        # New installations keep the profile name until the first explicit rank selection.
        default_name = b"Kilobyte[Champ I]" if path.parent.name == "kilobyte" else b"Megabyte"
        data, count = re.subn(rb'(?m)^name = "[^"\r\n]*"', b'name = "' + default_name + b'"', data)
        if count != 1: raise ValueError("Expected one default bot name in the profile bot.toml")
    return data


def package_bot(project: Path, output: Path) -> int:
    project, output = project.resolve(), output.resolve()
    if output.suffix.lower() != ".zip":
        raise ValueError("The output filename must end in .zip.")
    files = submission_files(project, output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Replace an earlier submission only after the new archive has been written successfully.
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".zip.tmp", delete=False) as temporary:
        temporary_path = Path(temporary.name)
    try:
        with ZipFile(temporary_path, "w", compression=ZIP_DEFLATED, compresslevel=6) as archive:
            for path in files:
                archive.writestr(path.relative_to(project).as_posix(), submission_content(project, path))
        temporary_path.replace(output)
    finally:
        temporary_path.unlink(missing_ok=True)
    return len(files)


def main(argv=None) -> int:
    project = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=project / "submission.zip", help="Output zip (default: submission.zip in the project root)")
    args = parser.parse_args(argv)
    try:
        count = package_bot(project, args.output)
        print(f"Created {args.output.resolve()} ({count} files). Send this zip to the host.")
        return 0
    except (OSError, ValueError) as error:
        print(f"Packaging error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
