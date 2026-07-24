# PalVerify

PalVerify is a paired client/server verification and anti-cheat project for a
Windows Palworld dedicated server.

## Platform policy

| Platform | Policy |
|----------|--------|
| Steam Windows | PalVerify required |
| Microsoft Store / Game Pass PC (`WinGDK`) | PalVerify required |
| Mac | PalVerify required |
| Linux / Proton | PalVerify required |
| PlayStation 5 (`PS5Base`, `PS5Trinity`) | Exempt |
| Confirmed Xbox console hardware | Exempt |
| Unknown or unmapped | PalVerify required |

The platform value must come from the authenticated server connection. A client
claiming that it is an Xbox is never enough for exemption.

Process-name findings are supplemental signals, not proof that a machine is
clean: a renamed tool can bypass an exact-name rule, so production enforcement
must combine them with signed client proof and server-authoritative gameplay
checks.

## Current status

Version 0.2.0 is installed locally and its observation-only server probe is
deployed on the BNB DatHost server. It is not ready for production enforcement
yet.

Implemented:

- Stable platform policy independent of Palworld enum integers.
- Current runtime mapping for `Windows`, `WinGDK`, `Linux`, `Mac`,
  `PS5Base`, `PS5Trinity`, and exact Xbox hardware models.
- Fail-closed runtime platform-name mapping.
- Initial challenge deadline and recurring heartbeat state machine.
- Protocol-version rejection and heartbeat replay detection.
- Windows running-process scanner with exact-name rules for WeMod and Cheat
  Engine.
- Privacy-minimized process findings that expose rule IDs instead of a process
  inventory.
- Native Windows launcher that detects the active Steam library, preserves
  existing `PalModSettings.ini` content, installs the client payload, and starts
  Palworld.
- Native Windows client agent that checks exact running-process image names
  every five seconds and stops when Palworld exits.
- Observation-only UE4SS Lua probe for platform/identity-related runtime symbols.
- Official Palworld package-layout draft.
- Reproducible Windows and server-observation ZIP packaging with SHA-256
  manifests and optional Authenticode signing.
- Per-user NSIS setup executable and MSI package with Start Menu integration,
  clean uninstall, major-upgrade handling, SHA-256 manifests, and optional
  Authenticode signing.

Not implemented yet:

- Per-connection Palworld platform adapter.
- Connection-bound RPC transport.
- Cryptographic proof verifier.
- Production kick adapter.
- Client-to-server reporting of local findings.
- Loaded-module rules and signed-manifest verification.
- Mac client runtime.

## Build and test

From PowerShell:

```powershell
.\scripts\build.ps1
.\scripts\package.ps1
.\scripts\package-installers.ps1
```

The script uses Visual Studio 2022's bundled CMake and builds the pure core
without requiring Palworld or UE4SS. The packaging script produces the Windows
launcher bundle and the server-observation ZIP under `dist`.

`package-installers.ps1` additionally creates:

- `PalVerify_0.2.0_x64-setup.exe`: recommended NSIS installer.
- `PalVerify_0.2.0_x64_en-US.msi`: Windows Installer alternative.
- `PalVerify_INSTALLER_SHA256SUMS.txt`: hashes for both installers.

Both installers use `%LOCALAPPDATA%\Programs\PalVerify`, require no
administrator privileges, and create a Start Menu shortcut. The NSIS finish
page can start PalVerify Launcher immediately; after an MSI install, launch it
from the Start Menu.

Installer build prerequisites:

```powershell
winget install --exact --id NSIS.NSIS
dotnet tool install --tool-path .deps\wix5 wix --version 5.0.2
```

WiX 5.0.2 is pinned so the build remains reproducible without automatically
accepting the WiX 6/7 OSMF EULA. Uninstalling the Windows application removes
the launcher and its bundled payload, but intentionally does not delete the mod
already installed inside Palworld or overwrite the settings backup.

The release executables are plain Microsoft Visual C++ binaries: they are not
packed, obfuscated, injected into another process, installed as a driver, or
made persistent. The packaging script signs them automatically when a valid
code-signing certificate exists. The current local build is unsigned because
this machine has no trusted code-signing certificate, so Windows SmartScreen
may still show a reputation warning even when Microsoft Defender finds no
malware.

## Server observation probe

The package under `packaging/server` is observation-only. It enumerates class
metadata and logs candidate property/function names related to platform,
identity, online subsystem, and device family. It does not inspect property
values, kick players, or enforce policy.

The BNB deployment loaded successfully on 2026-07-24. UE4SS logged PalVerify
v0.2.0 as `observation_only=true`, emitted candidate symbols for
`PlayerState`, `PlayerController`, `PalPlayerState`, and
`PalPlayerController`, then completed with `values_read=false`,
`enforcement=false`, and `pii_collected=false`.

Do not enable production kicks until real Steam, WinGDK, Linux, Mac, Xbox
console, and PS5 sessions have been observed on the target Palworld build.

## UE4SS native scaffold

Set `PALVERIFY_BUILD_UE4SS=ON` and point `PALVERIFY_UE4SS_ROOT` to a
Palworld-compatible RE-UE4SS source checkout. The native target is only a
lifecycle scaffold; the observation probe remains Lua until the correct remote
platform access path is validated.

RE-UE4SS's C++ build requires access to its Epic-licensed UEPseudo submodule.
The public Palworld zDev zip contains the runtime DLL and PDB but does not
replace that source dependency.
