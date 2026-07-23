# Pitfalls Research

- Generated enum integers are version-specific. Map by validated runtime symbol
  or adapter-owned constants and fail closed on unknown values.
- A global `GetPlatformType` function may describe the local executable rather
  than a remote player. Do not treat its existence as proof that remote platform
  classification is solved.
- WinGDK is a Windows PC build even though it authenticates in the Xbox
  ecosystem. Exempting it violates policy.
- UE4SS and Palworld updates can invalidate offsets, hooks, and generated
  headers. Add a startup compatibility gate.
- Heartbeat tokens prevent network replay but an attacker controlling the game
  process can still extract session material.
- Process names are easy to rename and legitimate tools can false-positive.
- Mac may not have a compatible native UE4SS client path. Requiring verification
  means Mac must fail closed until a supported client exists.
- Kick and ban must execute on the game thread using a supported server API.
- Do not run or redistribute unknown anti-cheat binaries found during research.
