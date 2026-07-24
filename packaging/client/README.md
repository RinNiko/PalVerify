# PalVerify client

The Windows package starts `PalVerifyClient.exe` through UE4SS. The agent scans
running process image names locally and records only stable PalVerify rule IDs
under `%LOCALAPPDATA%\PalVerify\PalVerifyClient.log`.

Version 0.2.0 remains observation-only: it does not transmit findings or prove a
server session yet. The connection-bound transport must be validated before
mandatory server kicks are enabled.

Mac is not exempt by policy, but a compatible Mac client adapter has not been
shipped yet.
