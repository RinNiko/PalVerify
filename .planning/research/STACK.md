# Stack Research

## Recommended

- C++20 for the deterministic core and native client/server adapters.
- CMake 3.25+ with MSVC 2022 on Windows.
- CTest with a dependency-free test executable for the first technical spike.
- Palworld-specific UE4SS native mod API for in-process client/server adapters.
- Palworld's official Workshop package metadata for distribution.
- Structured JSON Lines for evidence logs.

## Why

Native code is required for Windows process/module inspection and the UE4SS
native lifecycle. Keeping the policy and session state machine in standard C++
allows it to compile and test without loading Palworld or UE4SS.

## Local Toolchain

Visual Studio 2022 Community, its bundled CMake 3.31.6, Ninja 1.12.1, and Git are
available. CMake and Ninja are not on the default PATH, so build scripts must
resolve their Visual Studio bundled paths.

The Palworld-specific `UE4SS-Palworld_zDev.zip` runtime artifact is available
and its published SHA-256 was verified. It contains the runtime DLL, PDB, Lua
types, and tools, but not the C++ source/import target required to compile a C++
user mod.

## Dependency Boundary

The pure `palverify_core` target must not include UE4SS or generated Palworld
headers. Runtime adapters translate version-specific values into stable internal
types.

Windows process enumeration is isolated in `palverify_windows_integrity`.
Matching produces only stable rule identifiers; raw process inventories remain
inside the client process.

Building a UE4SS C++ user mod from source also requires access to the
Epic-licensed UEPseudo submodule. The authenticated GitHub account in this
environment does not currently have that repository access, so the native
lifecycle adapter remains a source scaffold.
