# Project State

## Current release

- Pal3Mien Launcher: `v1.0`
- PalVerify client/server: `v1.0`
- Palworld build: `24181527` (`v1.0.1.100619`)
- Public repository: `https://github.com/RinNiko/PalVerify`
- Permanent installer URL:
  `https://github.com/RinNiko/PalVerify/releases/download/stable/Pal3Mien-Setup.exe`
- Installer SHA-256:
  `67e1e685719b14de8e64d87939ec87601b8c6334164a621c74295f3314baa306`
- PalVerify package digest:
  `9b90101710eff6fc050ffb1fbdaa4f59dfcf8872c20fb6a90cb3f05abcefdd58`

## Production state

- The public GitHub repository contains only `README.md` and
  `palverify-launcher-manifest.json`.
- GitHub has one permanent release tagged `stable` with one asset:
  `Pal3Mien-Setup.exe`.
- The Palworld 3 Miền API accepts official two-part versions such as `1.0`,
  treats `1.0` and `1.0.0` as equal, and synchronizes the GitHub manifest.
- Production API change merged in `PalworldVN/Palworld-3-Mien-Website#2` and
  Vercel deployment `dpl_Eh4tjj5sbGRpAB6xXQ2QSaragc3H` is ready.
- PalVerifyServer v1.0 and `config.lua` v1.0 were uploaded to BNB over FTP.
  Remote round-trip SHA-256 matches the local artifacts.
- `server-config.json` points to the Palworld 3 Miền production API instead of
  the retired MineRua coordinator.
- The v1.0 GitHub manifest remains fail-safe with `serverOnline: false` until
  BNB is restarted and startup/evaluation logs confirm the new runtime.

## Verification

- Go coordinator/server tests pass.
- C++ build passes with `/W4 /WX`.
- Five CTest executables pass.
- Lua client/server probe tests pass.
- Windows installer package contract passes.
- NSIS produces the single fixed-name installer `Pal3Mien-Setup.exe`.
- Release layout contains no external `payload` directory and no PDB files.
- Authenticode remains unsigned because no valid code-signing certificate is
  installed on this machine.

## Next action

Restart BNB, verify `server agent started version=1.0` and successful
Palworld 3 Miền API evaluations, then publish the same v1.0 manifest with
`serverOnline: true`.
