# Praxis

Praxis is a Windows practice save tool for Elden Ring, Dark Souls III, Dark Souls Remastered, Dark Souls II: Scholar of the First Sin, and Sekiro: Shadows Die Twice. It creates organized backup libraries and restores full saves or active character slots with toolbar actions and global hotkeys.

## Supported Games

| Game | Save file | Backup extension |
|------|-----------|------------------|
| Elden Ring | `ER0000.sl2` | `.ersm` |
| Dark Souls III | `DS30000.sl2` | `.ds3sm` |
| Dark Souls Remastered | `DRAKS0005.sl2` | `.dsrsave` |
| Dark Souls II: Scholar of the First Sin | `DS2SOFS0000.sl2` | `.ds2save` |
| Sekiro: Shadows Die Twice | `S0000.sl2` | `.seksave` |

## Features

- Back up and restore full save files.
- Back up and restore the currently active character slot.
- Use global hotkeys while the game is focused.
- Keep separate game profiles for different games, accounts, installs, or save files.
- Keep multiple backup profiles under one game profile, each with its own named backup tree and compression level.
- Organize backups in a tree view with folders, sorting, rename, delete, and Show in File Explorer.
- Mark backup files read-only so they cannot be overwritten by Backup & Replace.
- Import existing full-save and slot-backup files into the active backup tree.
- Create a hidden `.praxis_ring/` pre-restore snapshot before restores, then undo the last restore if needed.
- Use System, Light, or Dark theme mode.
- Use the UI in 11 languages with automatic system language detection.

## Before You Use It

Close the game before restoring a backup. Praxis also shows this warning in the app because restoring while the game is writing the save can lose data.

Keep a normal manual backup of important save folders. Praxis creates safety snapshots before restores, but practice-save workflows intentionally overwrite live save files.

## Quick Start

1. Launch `Praxis.exe`.
2. Open **Game > Manage Game Profiles...**.
3. Add a game profile, choose the game, and set **Backup Root** to the folder where practice backups should live.
4. Set **Save File** only when auto-detection does not find the right save file or when you want to target a specific file.
5. Use the `+` button in the toolbar to add a backup profile. Its name becomes a subfolder under the game profile's backup root.
6. Select a backup profile from the toolbar dropdown.
7. Use the tree view to create or select the folder where new backups should be placed.

## Common Workflows

### Back Up the Current Save

Click **Backup Full Save** or press `Ctrl+Shift+F5`. Praxis creates a timestamped backup in the selected tree folder.

### Back Up the Active Character Slot

Load the character in game, then click **Backup Current Slot** or press `Ctrl+Shift+F6`. Praxis detects the active slot and writes a slot backup.

### Replace an Existing Backup

Select a backup file in the tree, then click **Backup & Replace** or press `Ctrl+Shift+F7`. Praxis overwrites that backup with the current full save or current active slot, matching the selected backup type.

Read-only backup files cannot be replaced. Right-click a file and use **Make read-only** or **Make writable** to change that lock.

### Restore a Backup

Select a backup file, then click **Restore** or press `Ctrl+Shift+F9`. Praxis detects whether the selected file is a full-save backup or a slot backup and restores it to the active game profile's save file.

Before the restore, Praxis writes a snapshot of the current live save into `.praxis_ring/` under the active backup profile folder.

### Undo the Last Restore

Click **Undo Last Restore** or press `Ctrl+Shift+Z`. Praxis restores the most recent `.praxis_ring/` snapshot.

### Move Through Nearby Saves

Use `Ctrl+Shift+Up` and `Ctrl+Shift+Down` to select the previous or next backup file in the current directory, using the current sort mode.

### Import Existing Backups

Use **File > Import as Original Format...** to import detected full-save backups as full saves and detected slot backups as slots.

Use **File > Import as Single Slot...** to import slot backups as slots and to extract the active character slot from detected full-save backups.

## Default Hotkeys

| Action | Default hotkey |
|--------|----------------|
| Backup Full Save | `Ctrl+Shift+F5` |
| Backup Current Slot | `Ctrl+Shift+F6` |
| Backup & Replace Selected Save | `Ctrl+Shift+F7` |
| Restore | `Ctrl+Shift+F9` |
| Undo Last Restore | `Ctrl+Shift+Z` |
| Previous Save in Directory | `Ctrl+Shift+Up` |
| Next Save in Directory | `Ctrl+Shift+Down` |

Change hotkeys from **Options > Hotkey Settings...**.

## Backup Tree

The backup tree mirrors folders and files on disk. Use the right-click menu to create folders, rename items, move files to the Recycle Bin, toggle read-only, or open a location in File Explorer.

The sort dropdown can order files by name or modified time, ascending or descending.

## Profiles

A game profile defines the game, the optional exact save file, and a backup root folder.

A backup profile belongs to one game profile. Its folder is created as:

```text
<Backup Root>\<Backup Profile Name>\
```

Example:

```text
C:\PraxisBackups\Elden Ring Main\Boss Practice\
```

Use separate game profiles for different games, different Steam accounts, or different modded save locations. Use separate backup profiles for routes, bosses, categories, or practice sessions.

## Existing Configurations

If Praxis finds an older single-tree configuration, it opens a one-time migration wizard. The wizard converts the old tree root into a game profile and backup profile while preserving the existing backup files in place.
