PalVerify Windows v0.2.0

1. Keep PalVerifyLauncher.exe and the payload folder together.
2. Close Palworld.
3. Run PalVerifyLauncher.exe.
4. The launcher detects the active Steam library, installs PalVerify, preserves
   existing PalModSettings.ini entries, and starts Palworld.

The first install creates:
  Mods\PalModSettings.ini.palverify-backup

Client log:
  %LOCALAPPDATA%\PalVerify\PalVerifyClient.log

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
