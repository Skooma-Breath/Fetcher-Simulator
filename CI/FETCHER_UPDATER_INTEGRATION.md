# Fetcher updater repository split

OpenMW no longer owns the Fetcher updater/tester-tools sources or stable prerelease. Those live in `Skooma-Breath/Fetcher-Updater`.

## Migration order

1. Publish and validate the new repository's `fetcher-tester-tools` prerelease from its routed commit.
2. Publish the OpenMW bridge commit `d6c83ffe2c` to the old `fetcher-tester-tools` prerelease. Its installed updater points all subsequent tester-tools checks to the already-available `Skooma-Breath/Fetcher-Updater` release while client checks remain on `Skooma-Breath/Fetcher-Simulator`.
3. Set the OpenMW repository variable `FETCHER_TESTER_TOOLS_SHA256` to the lowercase 64-character SHA-256 digest of the new `fetcher-tester-tools.zip` asset.
4. Run the OpenMW release workflow. Test-client packaging queries the new release, requires its GitHub digest to equal the pin, verifies the downloaded archive hash, validates safe paths, and verifies every manifest file hash and size.

The cleanup commit must not be deployed before steps 1 through 3 are complete. Existing testers continue to run `Update-Fetcher-Simulator.bat` without reinstalling.

## Local CI input

`CI/Package-WindowsClientMods.ps1` also accepts `-TesterToolsArchivePath` for a clearly supplied local CI artifact. The archive is still path-checked and manifest-verified. Pass `-TesterToolsSha256` as well when the local artifact has an external digest pin.

## Rollback

Replace the standalone stable prerelease with a known-good build and update `FETCHER_TESTER_TOOLS_SHA256` before packaging another test client. If the repository split must be rolled back, restore the OpenMW bridge commit and republish its four old-repository assets; installed filenames and the launch BAT remain unchanged.
