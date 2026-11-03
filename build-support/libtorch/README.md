Plain CPU Bob builds download the Windows SDK from the public RLBot CPU PyTorch
archive in `cpp.Dockerfile`. Both CPU archive downloads are release-pinned and
SHA-256 checked. The Python helper follows the same path unless a CPU SDK is
explicitly supplied through `--libtorch` or `LIBTORCH_CPU_ROOT`.

GPU builds require a local CUDA SDK. The helper finds it through `--libtorch`,
`LIBTORCH_CUDA_ROOT`, or the existing bot-pack installation. Supplied SDKs are
staged in `build-support/libtorch/local/`, containing headers, import libraries,
version and a content fingerprint. Runtime DLLs stay in the host's installation.

Use `--work-dir "path/to/scratch"` to stage an isolated source and SDK snapshot
outside the checkout instead. No drive letter is assumed.

The generated `local/manifest.json` stays visible to Bob's Git-aware hasher so
changing SDK versions invalidates cached builds. The temporary SDK copy is
removed after Bob exits. Any explicitly requested source snapshot remains for
diagnosis. Existing staging directories are never overwritten, and original SDK
files are never modified. See the repository README for build commands.
