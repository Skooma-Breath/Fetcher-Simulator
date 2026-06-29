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
  --version 2.0.1 `
  --applier .\CI\bardcraft-patch\Apply-Fetcher-Bardcraft-MPPatch.ps1 `
  --previous-manifest C:\path\to\previous\fetcher-bardcraft-mp-patch.json
```

`priorOutputSha256` records allow known previous patch outputs to upgrade. The
applier reconstructs modified upstream files from hash-verified pristine
backups and refuses unknown or locally modified script hashes.
