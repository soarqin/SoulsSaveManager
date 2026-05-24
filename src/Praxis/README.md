# Praxis

Praxis is a practice save tool for Elden Ring, Dark Souls III, Dark Souls Remastered, Dark Souls II: Scholar of the First Sin, and Sekiro: Shadows Die Twice that allows you to quickly backup and restore save files using global hotkeys. It features a tree-structured save library and automatic ring backups to ensure you never lose progress.

## Features

- **Full + Slot Backup/Restore** — Backup or restore the entire save file or just the currently active character slot.
- **Global Hotkeys** — Perform backups and restores instantly from within the game.
- **Tree-Structured Save Library** — Organize your saves in a hierarchical tree view. Rename, move, and delete saves with ease.
- **Recycle Bin Support** — Deleted saves are moved to the Windows Recycle Bin.
- **Ring Backup System** — Automatically maintains a 5-slot FIFO ring of backups every time you restore, allowing you to undo the last restore operation.
- **Multi-Game Ready** — Designed with a backend interface to support multiple games (currently implements Elden Ring, Dark Souls III, Dark Souls Remastered, Dark Souls II: Scholar of the First Sin, and Sekiro: Shadows Die Twice).

## Supported Games

| Game | Save File | Save Folder |
|------|-----------|-------------|
| Elden Ring | ER0000.sl2 | %APPDATA%\EldenRing\<decimal_steamid>\ |
| Dark Souls III | DS30000.sl2 | %APPDATA%\DarkSoulsIII\<hex16>\ |
| Dark Souls Remastered | DRAKS0005.sl2 | Documents\NBGI\DARK SOULS REMASTERED\<decimal_lower32>\ |
| Dark Souls II: Scholar of the First Sin | DS2SOFS0000.sl2 | %APPDATA%\DarkSoulsII\<hex16>\ |
| Sekiro: Shadows Die Twice | S0000.sl2 | %APPDATA%\Sekiro\<decimal_steamid>\ |

## Default Hotkeys

| Action | Default Hotkey |
|--------|---------------|
| Backup Full Save | Ctrl+Shift+F5 |
| Backup Current Slot | Ctrl+Shift+F6 |
| Backup & Replace Selected Save | Ctrl+Shift+F7 |
| Restore (auto-detect full or slot) | Ctrl+Shift+F9 |
| Undo Last Restore | Ctrl+Shift+Z |
| Previous Save in Directory | Ctrl+Shift+Up |
| Next Save in Directory | Ctrl+Shift+Down |

## Tree Storage

Saves are stored in a tree structure on disk, mirroring the organization in the UI. This allows for easy management and categorization of practice states.

## Ring Backup

The Ring Backup system (located in the `.praxis_ring/` hidden directory) automatically captures the state of your save file before any restore operation. If you accidentally restore the wrong save, you can use the Undo command to revert to the previous state.

## Build & Run

Refer to the root [README.md](../../README.md) for build instructions. Once built, launch `Praxis.exe`.

## Backend Interface

Praxis uses a compile-time vtable (`game_backend_t`) defined in `src/Praxis/game_backend.h`. This allows the core logic to remain game-agnostic while specific backends (like `er_backend.c` for Elden Ring, `ds3_backend.c` for Dark Souls III, `dsr_backend.c` for Dark Souls Remastered, `ds2_backend.c` for Dark Souls II: Scholar of the First Sin, and `sekiro_backend.c` for Sekiro: Shadows Die Twice) handle the details of save file locations and slot manipulation.

## Profiles

Praxis 2.0 introduces a multi-profile system:

- **Game Profiles** — One entry per game/account combination. Each game profile has a name, game type, optional override for the save file, and a backup root directory.
- **Backup Profiles** — Subordinate to a game profile. Each backup profile defines a tree root directory and compression level. You can have multiple backup profiles per game (e.g., `Main`, `SpeedrunPractice`, `PvP`).

### Profile Hierarchy

```
Game Profile: "Elden Ring - Main Account"
└── Backup Profile: "Main"    → C:\Backups\ER-Main\
└── Backup Profile: "Route B" → C:\Backups\ER-Main\RouteB\
```

Profiles are managed via the **Game** menu → **Manage Game Profiles** (for game profiles) or the `+` / `−` buttons on the toolbar (for backup profiles).

## Migration

If you are upgrading from a previous version of Praxis, a one-time migration wizard will automatically launch on first run. It will convert your existing `[Settings].TreeRoot` configuration into a game/backup profile pair, preserving all existing backup files in place.
