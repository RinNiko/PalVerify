# Project State

## Project Reference

See: `.planning/PROJECT.md` (updated 2026-07-24)

**Core value:** Required PC and Mac clients cannot remain connected without a
current PalVerify session, while real Xbox consoles and PS5 retain crossplay.

**Current focus:** Phase 3 - Live Per-Connection Platform Adapter

## Status

- Project initialized and committed.
- Pure C++ platform/session core implemented through observed RED-GREEN TDD.
- Release build passes 13 C++ tests with MSVC warnings treated as errors.
- Windows process scanning compiles with exact-name WeMod and Cheat Engine rules
  that return only minimized rule identifiers.
- Windows launcher and client agent v0.2.0 build with MSVC `/W4 /WX`; installer
  discovery, VDF parsing, settings preservation, payload installation, and
  client probe smoke tests pass.
- Launcher installed the client payload into the active Steam Palworld
  installation and created the first-install settings backup.
- Lua symbol filter passes 4 tests and the observation probe parses cleanly.
- Official package metadata resolves every target and thumbnail.
- Windows and server-observation release ZIPs are reproducibly packaged with
  SHA-256 output. Defender command-line scans reported no detections.
- Per-user NSIS and MSI installers build reproducibly with NSIS 3.12 and WiX
  5.0.2. Both formats were installed, payload-hash verified, used to run the
  launcher, and uninstalled successfully; the NSIS build is installed on the
  current machine.
- BNB DatHost has timestamped `mods.txt` and `mods.json` backups, the PalVerify
  server probe enabled in both lists, and all uploaded files were downloaded
  and SHA-256 verified.
- DatHost was stopped and started with zero players online. UE4SS confirmed
  PalVerify v0.2.0 loaded and completed its observation probe with enforcement
  and PII collection disabled.
- Palworld zDev runtime artifact was downloaded and SHA-256 verified.
- Native UE4SS source build requires GitHub access to Epic-licensed UEPseudo;
  the lifecycle source scaffold is present but cannot be compiled in this
  environment without that access.
- Runtime candidate symbols are now confirmed, but per-connection platform
  values remain unverified.
- The release is unsigned because no trusted code-signing certificate is
  installed on this machine; SmartScreen reputation warnings cannot be
  guaranteed away without a trusted signing certificate and reputation.

## Decisions

- Exempt exact Xbox console models and PS5 only.
- Require Steam Windows, WinGDK/Game Pass PC, Linux, and Mac.
- Unknown platforms fail closed.
- Keep runtime adapter values outside the pure core.
- Start enforcement in observation mode.

## Next Evidence Needed

1. Determine whether `PalPlayerState.SyncPlayerPlatformCache`,
   `OverridePlayerPlatform`, or `PlayerController.GetPlatformUserId` exposes a
   server-authenticated per-player platform value.
2. Capture those values during real connections without logging account or
   platform identifiers.
3. Implement and authenticate connection-bound client reporting.
4. Test real Steam, WinGDK, Linux, Mac, Xbox console, and PS5 sessions before
   enabling any kick action.
