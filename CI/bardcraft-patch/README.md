# Fetcher Bardcraft patch tooling

This directory contains the versioned builder and runtime applier for the
Fetcher Bardcraft multiplayer compatibility patch. The generated archive does
not contain a standalone copy of Bardcraft.

The builder requires:

- an unmodified Nexus Bardcraft `scripts/Bardcraft` directory;
- the Fetcher-modified client script directory;
- the previous released patch manifest when building an in-place upgrade.

Example:

```powershell
python .\CI\bardcraft-patch\build_patch.py `
  --vanilla-root C:\path\to\vanilla\scripts\Bardcraft `
  --fetcher-root C:\path\to\fetcher\scripts\Bardcraft `
  --output-dir C:\path\to\patch-output `
  --version 2.0.4 `
  --applier .\CI\bardcraft-patch\Apply-Fetcher-Bardcraft-MPPatch.ps1 `
  --previous-manifest C:\path\to\previous\fetcher-bardcraft-mp-patch.json `
  --extra-file Bardcraft.omwscripts `
    C:\path\to\vanilla\Bardcraft.omwscripts `
    C:\path\to\fetcher\Bardcraft.omwscripts
```

`priorOutputSha256` records allow known previous patch outputs to upgrade. The
builder carries that ancestry forward transitively, so users can skip patch
releases without being rejected as locally modified. When repairing ancestry
from an older non-transitive manifest, pass every affected released manifest
with repeated `--previous-manifest` arguments. The applier reconstructs
modified upstream files from hash-verified pristine backups and refuses
unknown or locally modified script hashes.

Normal records target `scripts/Bardcraft`. `--extra-file` creates an explicit
`targetBase: data` record for files such as `Bardcraft.omwscripts`; target paths
are validated and cannot escape the Bardcraft data root.
