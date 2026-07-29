# PalHudBridge

`PalHudBridge` is the server-only transport for the PalHud protocol. It reads
the pure-Lua runtime snapshot written by `PalBooster` and sends targeted
`ClientMessage` payloads to connected players every five seconds.

The bridge intentionally contains no client HUD construction, local-player
detection, hooks, or persistent UObject cache. Client rendering remains in the
separately packaged `PalHud` mod.

Production layout:

```text
ue4ss/Mods/PalHudBridge/Scripts/main.lua
```

Keep the mixed client/server `PalHud` entry disabled in the dedicated server's
`mods.txt`, and enable `PalHudBridge` instead.
