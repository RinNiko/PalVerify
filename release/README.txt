PalVerify Windows v0.2.0

Recommended:
  Run PalVerify_0.2.0_x64-setup.exe, then choose Run PalVerify Launcher on the
  finish page.

MSI alternative:
  Run PalVerify_0.2.0_x64_en-US.msi, then open PalVerify Launcher from the
  Start Menu.

Portable ZIP:
1. Keep PalVerifyLauncher.exe and the payload folder together.
2. Close Palworld.
3. Run PalVerifyLauncher.exe.
4. The launcher detects the active Steam library, installs PalVerify, preserves
   existing PalModSettings.ini entries, and starts Palworld.

The first install creates:
  Mods\PalModSettings.ini.palverify-backup

Client log:
  %LOCALAPPDATA%\PalVerify\PalVerifyClient.log

Windows application location:
  %LOCALAPPDATA%\Programs\PalVerify

The Windows uninstaller removes the launcher application and bundled payload.
It intentionally does not delete the PalVerify files already installed inside
Palworld or restore PalModSettings.ini automatically.

Security:
  - Built with Microsoft Visual C++.
  - No packer, obfuscator, injector, kernel driver, persistence, or disk scan.
  - Process findings are reduced to stable rule IDs.
  - Compare EXE hashes with SHA256SUMS.txt before running.
  - This build is unsigned because no trusted code-signing certificate was
    available. Microsoft Defender reports no detection, but SmartScreen may
    still show a reputation warning. A trusted Authenticode certificate is
    required to reduce that warning reliably.

Version 0.2.0 is an observation release. Server kicks and network reporting are
disabled until real crossplay platform fields are verified.
