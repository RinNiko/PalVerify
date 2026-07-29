# PalVerify client

The Windows package starts `PalVerifyClient.exe` through `Pal3Mien.exe`.
The agent scans the Palworld mod directories and cached executable evidence
locally. Every five seconds it reports a compact mod ID, version, aggregate
SHA-256 digest, and stable integrity rule IDs to the PalVerify coordinator. It
does not transmit a file list, executable path, or process inventory.

Logs are written under `%LOCALAPPDATA%\PalVerify\PalVerifyClient.log`.

Version 1.0.15 enforces the exact server whitelist for Steam Windows players,
includes packages from Palworld's configured Workshop root in the compact mod
inventory, reports enabled nested UE4SS mods by compact folder name, uses a
one-time challenge bound to an active server session instead of a shared client
token, identifies current Wand/WeMod and renamed Cheat Engine processes, and
rejects WeMod-signed trainer modules loaded into Palworld. A suspicious loaded
module is reported with its basename, product description, company, signer,
digest, and matching rule so players and administrators can identify the
software without exposing a full path or process inventory.
Always start Palworld through Pal3Mien so the verifier starts first.
Xbox consoles and PlayStation 5 are exempt. Game Pass PC remains observation
only until the dedicated server exposes an authenticated way to distinguish it
from Xbox console hardware.
