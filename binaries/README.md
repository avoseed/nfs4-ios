# Local build inputs

Place the matching owner-provided `nfs4.exe`, `voodoo2a.dll`, and `eacsnd.dll` here for local recompilation. Their expected sizes and SHA-256 hashes are recorded in `ci/input-manifest.json`.

Raw inputs are ignored by Git. The owner workflow restores an authenticated encrypted input bundle using the private repository secret. Supply your own files and key to build a different checkout; no decryption key is published.
