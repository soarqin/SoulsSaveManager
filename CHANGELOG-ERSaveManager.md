# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Tools → Downpatch to 1.02.1 menu command. Writes game version `0x0B` to the
  summary slot, regulation version `0x9BCAF6` and the built-in 1.02.1
  `regulation.bin` to the regulation slot (BND4 entry 11), then recomputes
  slot MD5 checksums. The 1.02.1 `regulation.bin` is embedded in the binary.
- System, Light, and Dark theme modes with a theme submenu under Options and
  per-app theme glue module.
- High-contrast support, `WM_SYSCOLORCHANGE` handler, and button hover/press
  states for the theme system.
- Minimum window size constraint and unified main window / dialog margins and
  spacing.
- `--selftest` subcommands for diagnosis and CI: `make-bnd4-stub`,
  `make-min-valid-sl2`, `provision-save-folder`, `dump-active-slot`,
  `write-active-slot`, `downpatch-1-02-1`, and several compression / format
  detection helpers.

### Changed
- Source files relocated to `src/ERSaveManager/` subdirectory (no behavior
  change).
- Shared modules (`ersave`, `save_compress`, `file_dialog`, `locale_core`,
  `config_core`, `theme_core`) extracted into `src/common` as a static library
  consumed by both ERSaveManager and Praxis.

### Fixed
- COM initialized in the main thread so `IFileDialog` folder pickers work
  reliably.
- Null guards and bounds checks across the public `ersave` API.
- Background erase forced on theme change redraw; listview column-resize ghost
  lines eliminated via double-buffering.

## [1.0.0] - 2026-04-04

### Added

- Character slot management: view, import, export, and rename across 10 character slots.
- Face data management: import and export face data for up to 15 face slots.
- Built-in NPC face presets from Elden Ring base game and Shadow of the Erdtree DLC.
- Cross-save character import from another Elden Ring save file.
- Character detail panel displaying level, 8 attributes, runes held, deaths, and play time.
- Steam ID re-signing when the save folder does not match the save file's Steam ID.
- Multi-language UI supporting 11 languages with automatic system language detection.
- INI-based configuration persisting save folder, language, and window layout settings.
- Native Win32 GUI with visual styles.
- BND4 save file format parsing with MD5 checksum validation.

[Unreleased]: https://github.com/soarqin/ERSaveManager/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/soarqin/ERSaveManager/releases/tag/v1.0.0
