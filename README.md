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

This repository is in the verified-core and server-observation stages. It is not
ready for production enforcement yet.

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
- Observation-only UE4SS Lua probe for platform/identity-related runtime symbols.
- Official Palworld package-layout draft.

Not implemented yet:

- Per-connection Palworld platform adapter.
- Connection-bound RPC transport.
- Cryptographic proof verifier.
- Production kick adapter.
- Loaded-module rules and signed-manifest verification.
- Mac client runtime.

## Build and test

From PowerShell:

```powershell
.\scripts\build.ps1
```

The script uses Visual Studio 2022's bundled CMake and builds the pure core
without requiring Palworld or UE4SS.

## Server observation probe

The package under `packaging/server` is observation-only. It enumerates class
metadata and logs candidate property/function names related to platform,
identity, online subsystem, and device family. It does not inspect property
values, kick players, or enforce policy.

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
