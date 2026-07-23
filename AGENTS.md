# PalVerify contributor rules

- Target a Windows Palworld dedicated server and supported PalVerify clients.
- Exempt only server-confirmed Xbox console hardware and PS5.
- Require Steam Windows, WinGDK/Game Pass PC, Linux, Mac, and unknown platforms
  to verify.
- Never trust a client-supplied platform value.
- Keep the pure policy/session core independent of UE4SS and Palworld headers.
- Use test-driven development for every behavior change.
- Runtime adapter mismatches fail closed and disable unsafe enforcement.
- Do not add automatic permanent bans without explicit approval.
- Do not scan disks or upload complete process, module, or file inventories.
- Keep all text files UTF-8 and do not introduce unnecessary Unicode escape
  literals.
