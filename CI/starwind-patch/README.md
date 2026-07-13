# Fetcher Starwind compatibility patch

This release overlays the Fetcher-maintained Starwind vanilla and multiplayer compatibility files without modifying the Nexus archive installed by UMO.

Version 2 also quarantines Starwind's root menu, cursor, scroll, and book UI textures outside the active data paths. This keeps Starwind world assets available while preventing its total-conversion UI skin from leaking into the shared vanilla multiplayer login and game interface.

The patch is portable. Its applier receives the tester installation root and discovered Starwind data root as parameters; it contains no machine-specific paths. Installation is staged and swapped into `Data Files/fetcher-starwind-compat`, with payload sizes and SHA-256 hashes verified before any existing patch is replaced.

For testers migrating from the previously published updater, the install root can also be discovered by walking upward from the UMO-managed Starwind data directory. The release includes the legacy manifest alias expected by that updater and regenerates `openmw.cfg` after applying the overlay, so the first `Update-Fetcher-Simulator.bat` run completes the migration.

Build a release archive from the compatibility project's `build` directory:

```powershell
.\Build-FetcherStarwindPatch.ps1 `
  -CompatibilityBuildRoot C:\path\to\starwind-vanilla-compat\build `
  -OutputDirectory C:\path\to\release-output `
  -PatchVersion 2.0.0 `
  -SourceCommit 7070c4f
```

Only the final `StarwindRemasteredV1.15.esm`, `StarwindRemasteredPatch.esm`, and `Starwind Vanilla Compat` overlay are packaged. Numbered intermediate ESM build artifacts are excluded.
