"""Packaging regressions; set BOB_EXE and PACKAGE_PROBE to enable integration checks.

Set BYTE_PACKAGE_TEST_ROOT to an E: scratch directory on Windows. Fixtures are
retained there so failed split output and model files can be inspected.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from check_package import PLATFORMS, check_package


PROJECT = Path(__file__).resolve().parents[1]
BOB = os.environ.get("BOB_EXE")
PROBE = os.environ.get("PACKAGE_PROBE")
SOURCE_LICENSES = (
    "LICENSE", "cpp-interface/LICENSE", "inc/RocketSim/LICENSE",
    "inc/RocketSim/libsrc/bullet3-3.24/LICENSE.txt",
)


class PackageTests(unittest.TestCase):
    def setUp(self):
        scratch = os.environ.get("BYTE_PACKAGE_TEST_ROOT")
        if not scratch:
            self.skipTest("Set BYTE_PACKAGE_TEST_ROOT to retain package test fixtures")
        Path(scratch).mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(prefix="package-", dir=scratch)).resolve()
        self.source = self.root / "source/rlbot"
        self.source.mkdir(parents=True)
        for model in ("kilobyte", "megabyte"):
            for name in ("POLICY", "SHARED_HEAD"):
                relative = f"{model}/{name}.lt"
                data = f"fixture {model} {name}".encode()
                path = self.source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        for profile in ("kilobyte", "megabyte"):
            (self.source / profile).mkdir(exist_ok=True)
            agent = "KilobyteReduced" if profile == "kilobyte" else "Megabyte"
            (self.source / profile / "bot.toml").write_text(
                f'[settings]\nagent_id = "SubparNova/{agent}"\nname = "old rank"\n')
        for name in ("loadout.toml", "logo.png"):
            (self.source / name).write_text("fixture")
        (self.source / "device.txt").write_text("cpu")
        for name in ("README.md", *SOURCE_LICENSES):
            path = self.source.parent / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
        self.build = self.root / "bob_build"
        self.package = self.build / "KilobyteReduced"
        self.package.mkdir(parents=True)
        (self.build / "buildinfo.toml").write_text(
            '[[projects]]\nname = "KilobyteReduced"\nhash = "1"\n'
            'build_date = 2026-11-03T00:00:00Z\n'
            '[[projects]]\nname = "Megabyte"\nhash = "2"\n'
            'build_date = 2026-11-03T00:00:00Z\n')

    def stage(self, platforms=None, gpu=False, profile="kilobyte"):
        if gpu:
            (self.source / "device.txt").write_text("gpu")
        directories = [(self.package / p, PLATFORMS[p]) for p in platforms] if platforms else [
            (self.package, "GGLBot.exe")]
        for directory, launcher in directories:
            (directory / "000-runtime").mkdir(parents=True)
            (directory / launcher).write_text("launcher fixture")
            suffix = ".exe" if launcher.endswith(".exe") else ""
            core = "GGLBotCoreCUDA" if gpu else "GGLBotCoreCPU"
            (directory / f"000-runtime/{core}{suffix}").write_text("core fixture")
            if profile == "kilobyte":
                (directory / f"000-runtime/ByteSkillUI{suffix}").write_text("GUI fixture")
        command = ["cmake", f"-DSOURCE_PACKAGE={self.source}", f"-DDEST_PACKAGE={self.package}",
                   f"-DBOT_PROFILE={profile}"]
        if platforms:
            command.append("-DPACKAGE_PLATFORMS=" + ";".join(platforms))
        command += ["-P", str(PROJECT / "cmake/StagePackage.cmake")]
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)

    def test_flat_native_package(self):
        self.stage()
        self.assertEqual(check_package(self.package), [self.package])
        self.assertIn('name = "Kilobyte[Champ I]"', (self.package / "bot.toml").read_text())
        self.assertFalse((self.package / "profile.txt").exists())
        self.assertFalse(any(self.package.rglob("manifest.json")))
        self.assertFalse(any(self.package.rglob("LICENSE*")))
        self.assertFalse((self.package / "cpp-interface").exists())
        self.assertFalse((self.package / "inc").exists())

    def test_native_deploy_into_source_profile_keeps_model_files(self):
        self.package = self.source / "kilobyte"
        before = {name: (self.package / name).read_bytes()
                  for name in ("POLICY.lt", "SHARED_HEAD.lt")}
        self.stage()
        self.assertEqual(check_package(self.package), [self.package])
        for name, content in before.items():
            self.assertEqual((self.package / name).read_bytes(), content)
        self.assertFalse(any(self.source.rglob("manifest.json")))
        self.assertFalse((self.package / "models").exists())

    def test_megabyte_contains_only_its_policy(self):
        self.stage(profile="megabyte")
        self.assertEqual(check_package(self.package), [self.package])
        self.assertIn('name = "Megabyte"', (self.package / "bot.toml").read_text())
        self.assertFalse((self.package / "kilobyte").exists())

    def test_reject_unknown_profile(self):
        self.stage()
        path = self.package / "bot.toml"
        path.write_text(path.read_text().replace("SubparNova/KilobyteReduced", "SubparNova/Byte"))
        with self.assertRaisesRegex(ValueError, "Unknown bot agent_id"):
            check_package(self.package)

    def test_legacy_profile_file_is_ignored(self):
        self.stage()
        (self.package / "profile.txt").write_text("megabyte")
        self.assertEqual(check_package(self.package), [self.package])

    def test_platform_configuration_must_not_shadow_shared_config(self):
        self.stage(list(PLATFORMS))
        path = self.package / "x86_64-unknown-linux-gnu/bot.toml"
        shutil.copy2(self.package / "bot.toml", path)
        with self.assertRaisesRegex(ValueError, "shadow"):
            check_package(self.package)

    def test_staging_rejects_missing_or_empty_models(self):
        policy = self.source / "kilobyte/POLICY.lt"
        policy.rename(policy.with_suffix(".missing"))
        with self.assertRaises(subprocess.CalledProcessError) as missing:
            self.stage()
        self.assertIn("Missing model", missing.exception.stderr)
        policy.write_bytes(b"")
        self.package = self.build / "EmptyModel"
        with self.assertRaises(subprocess.CalledProcessError) as empty:
            self.stage()
        self.assertIn("Empty model", empty.exception.stderr)

    def test_reduced_windows_package_requires_gui(self):
        self.stage()
        gui = self.package / "000-runtime/ByteSkillUI.exe"
        gui.rename(gui.with_suffix(".missing"))
        with self.assertRaisesRegex(ValueError, "ByteSkillUI.exe"):
            check_package(self.package)

    def test_reduced_linux_package_requires_gui(self):
        self.stage(["x86_64-unknown-linux-gnu"])
        gui = self.package / "x86_64-unknown-linux-gnu/000-runtime/ByteSkillUI"
        gui.rename(gui.with_suffix(".missing"))
        with self.assertRaisesRegex(ValueError, "ByteSkillUI"):
            check_package(self.package)

    def test_reject_leftover_policy(self):
        self.stage()
        extra = self.package / "models/byte/POLICY.lt"
        extra.parent.mkdir(parents=True)
        extra.write_text("obsolete")
        with self.assertRaisesRegex(ValueError, "Unexpected model"):
            check_package(self.package)

    def test_windows_only_gpu_package(self):
        self.stage(["x86_64-pc-windows-msvc"], gpu=True)
        self.assertEqual(check_package(self.package), [self.package / "x86_64-pc-windows-msvc"])
        self.assertFalse((self.package / "models").exists())
        self.assertFalse((self.package / "x86_64-unknown-linux-gnu").exists())

    def test_bad_platform_model_does_not_use_shared_models(self):
        self.stage(list(PLATFORMS))
        for name in ("POLICY.lt", "SHARED_HEAD.lt"):
            shutil.copy2(self.package / "x86_64-pc-windows-msvc" / name, self.package / name)
        policy = self.package / "x86_64-pc-windows-msvc/POLICY.lt"
        policy.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "Empty model"):
            check_package(self.package)

    @unittest.skipUnless(BOB, "Set BOB_EXE to run the actual Bob splitter")
    def test_bob_split_keeps_models_without_license_directories(self):
        self.stage(list(PLATFORMS))
        self.assertFalse((self.package / "models").exists())
        self.package = self.build / "Megabyte"
        self.stage(list(PLATFORMS), profile="megabyte")
        subprocess.run([BOB, "split", str(self.build)], check=True,
                       capture_output=True, text=True, timeout=60)
        for target, platform in (("x86_64-windows", "x86_64-pc-windows-msvc"),
                                 ("x86_64-linux", "x86_64-unknown-linux-gnu")):
            for project, profile in (("KilobyteReduced", "kilobyte"), ("Megabyte", "megabyte")):
                package = self.root / f"bob_build_{target}/{project}"
                self.assertEqual(check_package(package), [package / platform])
                self.assertFalse((package / platform / "bot.toml").exists())
                self.assertFalse((package / "models").exists())
                self.assertFalse(any(package.rglob("LICENSE*")))
                self.assertFalse((package / platform / "cpp-interface").exists())
                self.assertFalse((package / platform / "inc").exists())
                self.assertFalse(any(package.rglob("manifest.json")))
                for name in ("POLICY.lt", "SHARED_HEAD.lt"):
                    self.assertEqual((package / platform / name).read_bytes(),
                                     (self.source / profile / name).read_bytes())

    @unittest.skipUnless(PROBE, "Set PACKAGE_PROBE to verify compiled path resolution")
    def test_compiled_paths_keep_shared_config_and_platform_models(self):
        self.stage(list(PLATFORMS))
        for platform in PLATFORMS:
            launcher_dir = self.package / platform
            for directory in (launcher_dir, launcher_dir / "000-runtime"):
                probe = directory / Path(PROBE).name
                shutil.copy2(PROBE, probe)
                subprocess.run([str(probe), str(self.package), str(launcher_dir)],
                               cwd=self.root, check=True, capture_output=True, text=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
