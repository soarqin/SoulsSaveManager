# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-06-03

### Added

- Multi-game practice save tool for Elden Ring, Dark Souls III, Dark Souls Remastered, Dark Souls II: Scholar of the First Sin, and Sekiro: Shadows Die Twice.
- Game profiles for separate games, accounts, installs, or exact save-file targets.
- Backup profiles under each game profile, with named backup trees and per-profile compression settings.
- Tree-structured backup library with folders, sorting, rename, Recycle Bin delete, Show in File Explorer, and read-only lock toggles.
- Toolbar actions and global hotkeys for Backup Full Save, Backup Current Slot, Backup & Replace, Restore, Undo Last Restore, Previous Save, and Next Save.
- Full-save and active-slot backup/restore workflows with automatic backup type detection during restore.
- Pre-restore `.praxis_ring/` safety snapshots and Undo Last Restore support.
- Batch import tools for bringing existing full-save and slot backups into the active backup tree as original-format backups or single-slot backups.
- Multi-language UI supporting 11 languages with automatic system language detection.
- System, Light, and Dark theme modes.

[1.0.0]: https://github.com/soarqin/ERSaveManager/releases/tag/praxis-v1.0.0
