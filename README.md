# ERSaveManager + Praxis — Souls Save Tools

A collection of lightweight Windows desktop applications for managing Souls games save files.

- **ERSaveManager** — Import and export ELDEN RING character slots, manage face data, apply built-in NPC face presets, and re-sign Steam IDs.
- **Praxis** — A practice save tool with global hotkeys, tree-structured save library, and automatic ring backups. Supports **multi-profile** configurations (multiple game accounts/versions per game, multiple backup directories per profile) with auto-refresh via filesystem watcher.

## Documentation

- [ERSaveManager README](src/ERSaveManager/README.md) — Detailed features and usage for the ELDEN RING save manager.
- [Praxis README](src/Praxis/README.md) — Detailed features and usage for the practice tool.

## Building from Source

### Prerequisites

- [CMake](https://cmake.org/) 3.21 or later
- **MSVC** (Visual Studio 2022) or **MinGW** / **LLVM** toolchain with [Ninja](https://ninja-build.org/)
- Windows SDK

### Configure

```powershell
# MSVC (Visual Studio)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# MinGW / LLVM (single-config)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
```

### Build

```powershell
# Build everything
cmake --build build --config Release

# Build specific target
cmake --build build --config Release --target saveman  # ERSaveManager.exe
cmake --build build --config Release --target praxis   # Praxis.exe
```

The output binaries are located at `build/bin/`.

## License

This project is licensed under the [MIT License](LICENSE).
