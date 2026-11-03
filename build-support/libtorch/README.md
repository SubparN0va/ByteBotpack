The bob build helper temporarily creates `build-support/libtorch/local/` in the
checkout, matching the upstream helper. It contains the selected SDK's headers,
import libraries, version and content fingerprint. Runtime DLLs remain in the
host's shared bot pack.

Use `--work-dir "path/to/scratch"` to stage an isolated source and SDK snapshot
outside the checkout instead. No drive letter is assumed.

The generated `local/manifest.json` stays visible to bob's Git-aware hasher so
changing SDK versions invalidates cached builds. The temporary SDK copy is
removed after bob exits. Any explicitly requested source snapshot remains for
diagnosis. Existing staging directories are never overwritten, and original SDK
files are never modified. See the repository README for build commands.
