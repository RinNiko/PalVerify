# Research Summary

PalVerify should start as a portable C++ policy/session core with thin,
version-specific UE4SS adapters. Current community headers strongly suggest that
Windows, WinGDK, Mac, PS5, and Xbox hardware models can be represented
separately, which makes the requested policy plausible. The critical unknown is
whether the Windows dedicated server exposes that value per remote connection
early enough to enforce verification.

The safest first milestone is therefore:

1. Lock the platform policy in unit tests.
2. Implement challenge/heartbeat timeout behavior in a pure state machine.
3. Add an adapter contract that cannot exempt Unknown or WinGDK.
4. Build an UE4SS server probe that logs the runtime platform value without
   kicking anyone.
5. Enable kicks only after Steam, WinGDK, Xbox console, PS5, and Mac observations
   are validated on the target game build.
