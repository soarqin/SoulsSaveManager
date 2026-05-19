# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

> **Note:** Hotkey configuration migration is not provided; software was never released,
> so all users start with default hotkeys.

### Added
- Dark Souls III save backup/restore support in Praxis
- New `ds3save` module in `src/common/` providing the Praxis-runtime save API for Dark Souls III
- New `ds3_backend` registered in the Praxis backend registry alongside the existing Elden Ring backend
- DS3-specific selftest subcommands (`ds3-aes-known-vector`, `ds3-load-min-fixture`, `ds3-roundtrip-byte-stable`, `ds3-active-slot`, `ds3-null-guards`, `ds3-import-resigns-userid`, `ds3-real-save-load`, `ds3-real-save-classify`, `ds3-real-save-roundtrip-readonly`)
- Praxis: Multi-profile support — game profiles (per-account/version) and backup profiles (per game) via `Praxis.ini` multi-section schema
- Praxis: Game Profile Manager dialog (modal ListView) for managing game configurations
- Praxis: Toolbar with backup profile combobox + Backup Full / Backup Slot / Restore / Undo Restore buttons
- Praxis: Save tree sort selector with filename/modified-time ordering and ascending/descending modes
- Praxis: Read-only backup file toggle from the save tree context menu, including locked display marker and Backup & Replace gating
- Praxis: Filesystem watcher with auto-refresh (ReadDirectoryChangesW worker thread, 200ms debounce) and selection preservation
- Praxis: First-launch profile setup prompt that creates a default game profile when `Praxis.ini` has no profiles
- Praxis: 3 compression levels (none / low / high) per backup profile
- Praxis: `Praxis.exe --selftest locale-dump`, `watcher-state`, and other new headless QA subcommands
- Praxis: New `--selftest` subcommands: `config-load`, `hotkey-defaults`, `backend-vtable-shape`, `profile-resolve-active`, `watcher-debounce-timing`
- Praxis: New `--selftest` subcommands for sorted sibling navigation, read-only toggling, read-only folder rejection, and read-only Backup & Replace rejection

### Changed (BREAKING)
- `backend-default-save-dir` selftest now accepts a `<game_id>` argument (1 = Elden Ring, 2 = Dark Souls III)
- `docs/DS3SaveFormatResearch.md` Steam ID width corrected from 16 bytes to 8 bytes (was a documentation error)
- Praxis: `restore_with_safety()` renamed to `restore_safe_full()`
- Praxis: `restore_with_safety_auto()` renamed to `restore_safe_auto()`
- Praxis: `undo_last_restore()` renamed to `restore_safe_undo()`
- Praxis: `show_hotkey_settings()` renamed to `dialog_hotkey_settings_show()`
- Praxis: `show_game_profile_manager()` renamed to `dialog_game_profile_manager_show()`
- Praxis: `edit_game_profile()` renamed to `dialog_edit_game_profile_show()`
- Praxis: `edit_backup_profile()` renamed to `dialog_edit_backup_profile_show()`
- Praxis: Restore is now a single unified action (auto-detects full vs slot save from backup file header)
- Praxis: Hotkey count reduced from 5 to 4 — `HOTKEY_RESTORE` replaces `HOTKEY_RESTORE_FULL` + `HOTKEY_RESTORE_SLOT`; unified default: `Ctrl+Shift+F9`
- Praxis: Cyclic backup (pre-restore snapshot) is explicitly always a full save
- Praxis: New ring backup files use `.sl2` extension (raw BND4); existing `.ersm` ring files still readable

### Removed (BREAKING)
- Praxis: Legacy INI migration logic removed (software was still pre-release)
- Praxis: `migration-detect` / `migration-run` selftest subcommands removed
- Praxis: 7-argument `restore_with_safety` signature (replaced by `restore_safe_params_t` struct)
- Praxis: Backup and Restore main-menu entries (now toolbar buttons)
- Praxis: File→Refresh menu item (auto-refresh via filesystem watcher)

### Refactored
- Praxis: Selftest dispatcher extracted to `praxis_selftest.c/h` (was inline in `main.c`)
- Praxis: Hotkey action handlers extracted to `praxis_hotkey_actions.c/h`
- Praxis: Dynamic main menu logic extracted to `praxis_main_menu.c/h`
- Praxis: Save tree `WM_NOTIFY` handler extracted to `save_tree_notify.c/h`
- Praxis: Profile store INI persistence extracted to `profile_store_io.c/h`
- Praxis: BND4 selftest constants consolidated in `bnd4_test_format.h`
- Praxis: Save tree internal types consolidated in `save_tree_internal.h`

### Fixed
- Praxis: Pre-existing `.ersm` extension on raw-BND4 ring snapshots was misleading (new files now use `.sl2`)
- Praxis: Backup & Replace now rejects read-only selected files across toolbar, hotkey, and action-layer code paths

### Build
- `ersave_common` static library now links `bcrypt` (Windows CNG) PUBLIC for AES-128-CBC used by DS3 save handling

## [1.1.0] - 2026-04-25

### Added
- Praxis: Practice save tool with global hotkeys (Ctrl+Shift+F5/F6/F9/F10/Z)
- Praxis: Tree-structured save library with rename, move, delete (Recycle Bin)
- Praxis: Auto ring backup (5-slot FIFO) with undo last restore
- Praxis: Game backend interface (compile-time vtable, `game_backend_t`)
- Praxis: Elden Ring backend implementing full-save + slot backup/restore

### Changed
- ERSaveManager: Source files relocated to `src/ERSaveManager/` subdirectory (no behavior change)
- Repository: Shared `src/common/` static library extracted (ersave, save_compress, file_dialog, locale_core, config_core)

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
