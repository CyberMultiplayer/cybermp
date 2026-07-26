# cybermp

Multiplayer foundation for Cyberpunk 2077, built as a [RED4ext](https://github.com/WopsS/RED4ext) plugin.

[![build](https://github.com/yutho-o/cybermp/actions/workflows/build.yml/badge.svg)](https://github.com/yutho-o/cybermp/actions/workflows/build.yml)

---

## Requirements

| Tool | Version |
|------|---------|
| Windows | 10 / 11 (x64) |
| CMake | ≥ 3.21 |
| Visual Studio | 2022 (with MSVC and C++ workload) |
| Git | any recent version |
| Cyberpunk 2077 | 2.31 (with RED4ext ≥ 0.6 installed) |

> **Note** — cybermp targets **Windows x64 only**. The build will fail early with a clear message on other platforms or pointer widths.

---

## Getting started

### 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/yutho-o/cybermp.git
cd cybermp
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

### 2. Configure

```bash
cmake -S . -B build -A x64
```

To also enable the `deploy` target (copies the DLL straight into your game install):

```bash
cmake -S . -B build -A x64 -DCYBERMP_GAME_DIR="D:/Cyberpunk 2077"
```

### 3. Build

```bash
cmake --build build --config Release
```

The output is produced at `build/src/client/Release/cybermp.dll` (and a matching `.pdb`).

### 4. Deploy (optional)

If `CYBERMP_GAME_DIR` was set during configure, run:

```bash
cmake --build build --target deploy
```

This copies `cybermp.dll` to `<CYBERMP_GAME_DIR>/red4ext/plugins/cybermp/`.

Alternatively, copy the DLL there manually.

---

## Architecture

```
cybermp/
├── src/client/         # Plugin source (compiled into cybermp.dll)
│   ├── Main.cpp        # RED4ext entry points: Supports / Query / Main
│   ├── Plugin.cpp/.hpp # Holds the plugin handle and SDK pointer
│   ├── WorldSystem.*   # IGameSystem — fires when a world loads/unloads
│   ├── DebugApi.*      # RTTI bridge exposed to scripts as "Cybermp.Debug"
│   └── Log.hpp         # Thin logging helpers
├── vendor/
│   ├── RED4ext.SDK     # C++ SDK for RED4ext (submodule)
│   └── RedLib          # RTTI/script helpers on top of the SDK (submodule)
└── cmake/
    └── Deploy.cmake    # Optional deploy target
```

### Key concepts

- **RED4ext plugin** — the DLL exports `Supports`, `Query`, and `Main`. The loader calls them in that order during game startup/shutdown.
- **WorldSystem** — derives from `Red::IGameSystem`; RedLib registers it automatically. Lifecycle callbacks (`OnWorldAttached` / `OnWorldDetached`) are the earliest safe point to interact with the player and world.
- **DebugApi** — a `Red::IScriptable` struct registered as `Cybermp.Debug` in the game's RTTI, accessible from CET and Redscript via `GetSingleton`.

---

## CI

Every push and pull request triggers the [`build`](.github/workflows/build.yml) workflow on `windows-latest` (VS 2022). It:

1. Configures and builds in Release.
2. Verifies the DLL is x64 (`IMAGE_FILE_MACHINE_AMD64`).
3. Checks that the three required RED4ext exports are present and undecorated.
4. Uploads `cybermp.dll` + `cybermp.pdb` as a build artifact.

---

## License

This project does not yet have a license. Until one is added, all rights are reserved.
