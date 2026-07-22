# Fetcher updater repository split

OpenMW no longer owns the Fetcher updater/tester-tools sources or stable prerelease. Those live in `Skooma-Breath/Fetcher-Updater`.

## Unified client architecture

OpenMW publishes one clean Windows base client:

- release tag: `Fetcher-Simulator`
- asset: `fetcher-simulator.zip`
- channel marker: `clean`

The base archive contains OpenMW and portable configuration only. Fetcher-Updater owns the tester tools, UMO mod installation, public-test configuration, and compatibility patches. New testers run `Setup-Fetcher-Updater.bat`; existing testers continue running `Update-Fetcher-Simulator.bat`.

## Migration

The standalone updater must be published first with its client defaults set to `Fetcher-Simulator` and `fetcher-simulator.zip`. It accepts both legacy `test` and unified `clean` channel markers, so existing installations migrate without reinstalling. The old `Fetcher-Simulator-Test` prerelease remains available as a static fallback during the migration window but is no longer rebuilt.

## Rollback

Republish a known-good standalone updater if overlay routing must be rolled back. The `fetcher-updater-migration-bridge` branch and old-repository `fetcher-tester-tools` tag remain available for older installations. The retired `Fetcher-Simulator-Test` prerelease can be restored as an actively built channel by reverting the unified-client commit.
