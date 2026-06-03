# ERSaveManager

ERSaveManager is a Windows desktop tool for Elden Ring save editing. It focuses on character slots, face data, NPC face presets, and Steam ID re-signing through a native Win32 interface.

## Supported Save

- Game: Elden Ring
- Save file: `ER0000.sl2`
- Default location: `%APPDATA%\EldenRing\<decimal SteamID>\`
- Character slots: 10
- Face data slots: 15

## Features

- View character slots with name, body type, level, play time, attributes, runes held, and death count.
- Import, export, and rename character slots.
- Import a character slot from another full Elden Ring save file.
- Manage face data slots, including import and export.
- Apply built-in NPC face presets from the base game and Shadow of the Erdtree.
- Detect Steam ID mismatches between the save folder and save file, then offer to re-sign the save.
- Choose Fast, Normal, or Max compression for exported character files.
- Use System, Light, or Dark theme mode.
- Use the UI in 11 languages with automatic system language detection.

## Before You Use It

Close Elden Ring before importing, renaming, or re-signing save data. These operations write directly to the selected `ER0000.sl2` file.

Keep a manual backup of your save folder before editing. ERSaveManager can export individual character and face data, but it does not automatically create a full-save safety copy before every edit.

## Quick Start

1. Launch `ERSaveManager.exe`.
2. If the save folder is not detected correctly, click **Change Save Folder** and select the folder that contains your Steam ID subfolders.
3. Select a Steam ID folder from the dropdown. ERSaveManager loads that folder's `ER0000.sl2`.
4. Select a character in the **Characters** list to view its details.
5. Use **Import Character**, **Export Character**, or **Rename Character** for the selected slot.
6. Click **Face Data...** to manage face slots and NPC presets.

## Character Workflows

### Export a Character

1. Select an occupied character slot.
2. Click **Export Character**.
3. Choose a destination file.

The exported file can be imported back into ERSaveManager later.

### Import a Character File

1. Select the destination character slot.
2. Click **Import Character**.
3. Pick an exported character file.
4. Confirm the result in the character list.

The destination slot is overwritten.

### Import from Another Save

1. Select the destination character slot.
2. Click **Import Character**.
3. Pick another Elden Ring `ER0000.sl2` file.
4. Choose which character from that save to import.

## Face Data Workflows

1. Click **Face Data...**.
2. Select a face slot.
3. Use **Import Face Data** or **Export Face Data** for external files.
4. Use **Import NPC face data** to apply a built-in preset.

Imported face data overwrites the selected face slot.

## Steam ID Re-signing

When ERSaveManager loads a save, it compares the Steam ID inside the save file with the selected save folder name. If they differ, it asks whether to update the save file's Steam ID so Elden Ring can accept it in that folder.

## Options

- **Language** changes the UI language.
- **Options > Compression Ratio** changes exported character-file compression.
- **Options > Theme** selects System, Light, or Dark mode.
