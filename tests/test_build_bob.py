"""Check CPU download defaults and explicit SDK selection without running Docker."""
from pathlib import Path
import contextlib
import io
import os
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import build_bob


class BuildSdkTests(unittest.TestCase):
    def test_cpu_default_does_not_require_a_local_sdk(self):
        with patch.object(build_bob, "find_sdk", side_effect=AssertionError("Local SDK lookup")):
            self.assertIsNone(build_bob.select_sdk(Path.cwd(), "cpu", None, {}))

    def test_cuda_override_does_not_change_a_cpu_build(self):
        with patch.object(build_bob, "find_sdk", side_effect=AssertionError("CUDA SDK lookup")):
            self.assertIsNone(build_bob.select_sdk(
                Path.cwd(), "cpu", None, {"LIBTORCH_CUDA_ROOT": "cuda-sdk"}))

    def test_explicit_cpu_sdk_is_used(self):
        selected = (Path("cpu-sdk"), "2.4.1+cpu")
        with patch.object(build_bob, "sdk_version", return_value=selected[1]) as version:
            self.assertEqual(build_bob.select_sdk(Path.cwd(), "cpu", selected[0], {}), selected)
            version.assert_called_once_with(selected[0], "cpu")

    def test_invalid_cpu_environment_override_does_not_fall_back(self):
        with patch.object(build_bob, "sdk_version", side_effect=ValueError("Incomplete SDK")) as version:
            with self.assertRaisesRegex(ValueError, "Incomplete SDK"):
                build_bob.select_sdk(Path.cwd(), "cpu", None, {"LIBTORCH_CPU_ROOT": "bad-sdk"})
            version.assert_called_once_with(Path("bad-sdk"), "cpu")

    def test_gpu_requires_a_valid_sdk(self):
        with patch.object(build_bob, "sdk_version", side_effect=ValueError("Missing CUDA")):
            with self.assertRaisesRegex(ValueError, "GPU builds require"):
                build_bob.select_sdk(Path.cwd(), "gpu", Path("bad-sdk"), {})

    def test_gpu_environment_override_is_used(self):
        with patch.object(build_bob, "sdk_version", return_value="2.9.1+cu126") as version:
            result = build_bob.select_sdk(Path.cwd(), "gpu", None, {"LIBTORCH_CUDA_ROOT": "cuda-sdk"})
            self.assertEqual(result, (Path("cuda-sdk"), "2.9.1+cu126"))
            version.assert_called_once_with(Path("cuda-sdk"), "gpu")

    def test_cpu_helper_calls_bob_without_staging_and_preserves_failure(self):
        with patch.dict(os.environ, {}, clear=True), \
                patch.object(build_bob, "read_device", return_value="cpu"), \
                patch.object(build_bob.shutil, "which", return_value="bob"), \
                patch.object(build_bob, "staged_sdk", side_effect=AssertionError("Unexpected staging")), \
                patch.object(build_bob.subprocess, "run", return_value=subprocess.CompletedProcess([], 17)) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(build_bob.main(["--out-dir", "test-output"]), 17)
            self.assertEqual(run.call_args.args[0],
                             ["bob", "build", "bob.toml", "--out-dir", str(Path("test-output").resolve())])


if __name__ == "__main__":
    unittest.main()
