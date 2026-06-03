# AGENTS.md — ERSaveManager

A Windows GUI tool (Win32 API, C11) for managing Elden Ring save files: importing/exporting
face data and character slots, re-signing Steam IDs, and multi-locale UI.

### ⚠️ IMPORTANT: Language Requirements

**All code comments and documentation MUST be written in English.**

---

## Project Layout

```
ERSaveManager/
├── CMakeLists.txt          # Root: sets C++ standard to C23 (C std follows toolchain default)
├── README.md               # Project documentation (English)
├── CHANGELOG-ERSaveManager.md  # ERSaveManager changelog (Keep a Changelog format)
├── CHANGELOG-Praxis.md         # Praxis changelog
├── LICENSE                     # MIT License
├── .github/
│   └── workflows/
│       ├── release-saveman.yml # CI: ERSaveManager release on saveman-v* tags
│       └── release-praxis.yml  # CI: Praxis release on praxis-v* tags
├── cmake/
│   ├── CustomCompilerOptions.cmake   # /utf-8 flag, strip/LTO/static-CRT options
│   ├── GlobalOptions.cmake           # Visibility presets, export compile commands
│   └── ProjectMacros.cmake           # add_project() macro
├── deps/
│   ├── inih/               # INI file parser (linked but currently unused)
│   └── md5/                # MD5 hash library
└── src/                    # Main application source
    ├── CMakeLists.txt      # add_subdirectory(common/ERSaveManager/Praxis)
    ├── common/             # src/common: Static library: ersave, save_compress, file_dialog, locale_core, config_core, theme_core
    │   ├── theme_core.c/h  # Generic Win32 dark/light theme infrastructure (shared between apps)
    │   ├── dsrsave.c/h     # Dark Souls Remastered save handling
    │   ├── ds2save.c/h     # Dark Souls II: Scholar of the First Sin save handling
    │   ├── sekirosave.c/h  # Sekiro: Shadows Die Twice save handling
    │   └── CMakeLists.txt
    ├── ERSaveManager/      # src/ERSaveManager: ERSaveManager executable sources
    │   ├── theme.c/h       # Per-app dark/light theme glue module
    │   └── CMakeLists.txt
    └── Praxis/             # src/Praxis: Praxis executable sources
        ├── CMakeLists.txt
        ├── backends/
        │   ├── er_backend.c            # Elden Ring backend vtable instance
        │   ├── ds3_backend.c           # Dark Souls III backend vtable instance
        │   ├── dsr_backend.c           # Dark Souls Remastered backend vtable instance
        │   ├── ds2_backend.c           # Dark Souls II: Scholar of the First Sin backend vtable instance
        │   └── sekiro_backend.c        # Sekiro: Shadows Die Twice backend vtable instance
        ├── bnd4_test_format.h          # BND4 selftest constants (magic bytes, offsets)
        ├── dsr_test_format.h           # DSR-specific selftest constants
        ├── ds2_test_format.h           # DS2-specific selftest constants
        ├── sekiro_test_format.h        # Sekiro-specific selftest constants
        ├── praxis_selftest.c/h         # Selftest subcommand dispatcher (linked into PraxisSelftest only)
        ├── selftest_dsr.c/h            # DSR-specific selftest implementations
        ├── selftest_ds2.c/h            # DS2-specific selftest implementations
        ├── selftest_sekiro.c/h         # Sekiro-specific selftest implementations
        ├── selftest_main.c             # Console wmain() entry point for PraxisSelftest.exe
        ├── praxis_hotkey_actions.c/h   # Hotkey action handlers (backup/restore/undo)
        ├── praxis_main_menu.c/h        # Dynamic main menu construction
        ├── praxis_toast.c/h            # Centered, auto-fading notification panel (toast)
        ├── praxis_window_common.c/h    # Shared helpers for the Praxis main window
        ├── toolbar.c/h                 # Backup profile selector, file sort selector, and action buttons
        ├── save_tree_notify.c/h        # Save tree WM_NOTIFY handler
        ├── save_tree_internal.h        # Save tree internal types (shared between save_tree.c and save_tree_notify.c)
        ├── save_tree_walk.c            # Filesystem walking, sorting, and file attribute helpers for the save tree
        ├── theme.c/h                   # Per-app dark/light theme glue module
        ├── profile_store_io.c/h        # Profile store INI persistence (read/write Praxis.ini)
        └── ...
```

---

## Build Commands

This is a **CMake + C project targeting Windows only** (MSVC or MinGW).

### Configure (choose one generator)

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

### Build (Debug)

```powershell
cmake --build build --config Debug
```

### Clean rebuild

```powershell
# Build specific target
cmake --build build --config Release --target saveman         # ERSaveManager.exe
cmake --build build --config Release --target praxis          # Praxis.exe
cmake --build build --config Release --target praxis_selftest # PraxisSelftest.exe
```

### Notes
- **`PraxisSelftest.exe`** is a standalone console executable for automated testing.
  It is separate from `Praxis.exe` (GUI) to avoid Win32 console/stdout issues.
  Build it with `--target praxis_selftest`; output goes to `build/bin/PraxisSelftest.exe`.
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` is set automatically → `build/compile_commands.json`
  is generated for clangd/IDE tooling.

---

## Release & Versioning

ERSaveManager and Praxis are released independently using distinct Git tag prefixes.

| Product        | Tag Prefix    | Example           | Workflow File                  |
|----------------|---------------|-------------------|--------------------------------|
| ERSaveManager  | `saveman-v`   | `saveman-v1.2.0`  | `.github/workflows/release-saveman.yml` |
| Praxis         | `praxis-v`    | `praxis-v1.0.0`   | `.github/workflows/release-praxis.yml`  |

Pushing a matching tag triggers the corresponding workflow, which builds **only** that product's target, packages it with its product-specific `CHANGELOG` (renamed to `CHANGELOG.md` in the archive), `README.md`, and `LICENSE`, and publishes an independent GitHub Release.

### CHANGELOG Files

- `CHANGELOG-ERSaveManager.md` — ERSaveManager release history. The `[Unreleased]` section holds pending ERSaveManager/shared changes; `[1.0.0]` documents the initial release.
- `CHANGELOG-Praxis.md` — Praxis release history. New versions are appended here.

---

## Praxis-Specific Notes

- **Default Hotkeys**:
  - Backup Full Save: `Ctrl+Shift+F5`
  - Restore (auto-detect full or slot): `Ctrl+Shift+F9`
  - Backup Current Slot: `Ctrl+Shift+F6`
  - Backup & Replace Selected Save: `Ctrl+Shift+F7`
  - Undo Last Restore: `Ctrl+Shift+Z`
  - Previous Save in Directory: `Ctrl+Shift+Up`
  - Next Save in Directory: `Ctrl+Shift+Down`
- **Ring Backup Location**: `<tree_root>/.praxis_ring/` (hidden directory).
- **Backend Interface**: Compile-time vtable defined in `src/Praxis/game_backend.h`.
- **Save Tree Sorting**: Top toolbar sort combobox supports filename ascending/descending and modified-time ascending/descending.
- **Read-only Backups**: Save tree context menu can toggle read-only only for files. Read-only files show a locked marker; Backup & Replace is disabled for them and hotkey-triggered replacement is a no-op.
- **Selftest executable**: `PraxisSelftest.exe` — a standalone **console** program.
  - **Do NOT use `Praxis.exe --selftest`** — `Praxis.exe` is a Win32 GUI subsystem binary;
    its stdout is unreliable when launched from scripts and `$LASTEXITCODE` is not set.
  - Always use `PraxisSelftest.exe <subcommand> [args...]` directly.
- **Invoking selftest commands (PowerShell)**:
  ```powershell
  & .\build\bin\PraxisSelftest.exe <subcommand> [args...]
  # Example:
  & .\build\bin\PraxisSelftest.exe smoke
  & .\build\bin\PraxisSelftest.exe ds3-real-save-load "C:\path\to\DS30000.sl2"
  ```
  `$LASTEXITCODE` is reliable: `0` = pass, `1` = assertion failed, `2` = usage error.
- **⚠️ DESTRUCTIVE subcommands** — these write to and then DELETE the path passed as
  `<tmp>`. **Never pass a real save file path as `<tmp>`**; always use a throwaway path
  under `$env:TEMP`:
  - `ds3-load-min-fixture <tmp>` — creates fixture at `<tmp>`, asserts structure, **deletes `<tmp>`**
  - `ds3-roundtrip-byte-stable <tmp>` — creates fixture at `<tmp>`, round-trips, **deletes `<tmp>`**
  - `ds3-active-slot <tmp> <expected_int>` — creates fixture at `<tmp>`, checks slot, **deletes `<tmp>`**
  - `ds3-null-guards` — creates/deletes its own tmp internally (safe)
  - `ds3-import-resigns-userid <srcA> <dstB>` — creates fixtures at `<srcA>` and `<dstB>`, **deletes both**
  - `ds3-real-save-roundtrip-readonly <path> <tmp_copy>` — reads `<path>` (safe), writes/deletes `<tmp_copy>`
  - `dsr-load-min-fixture <tmp>` — build DSR fixture, load, assert structure ⚠️ destructive
  - `dsr-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `dsr-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `dsr-cross-account-import <srcA> <dstB>` — verify import works across accounts ⚠️ destructive
  - `ds2s-load-min-fixture <tmp>` — build DS2S fixture, load, assert structure ⚠️ destructive
  - `ds2s-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `ds2s-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `ds2s-dual-slot-roundtrip <tmp>` — verify dual-slot (part A + part B) serialization ⚠️ destructive
  - `ds2s-import-resigns-userid-text <srcA> <dstB>` — verify text Steam ID re-ownership on import ⚠️ destructive
  - `ds2s-available-slots-by-profile-byte <tmp>` — verify availability via profile int32 flag ⚠️ destructive
  - `ds2s-bnd4-entry-count <tmp>` — assert BND4 entry count == 23 ⚠️ destructive
  - `sekiro-load-min-fixture <tmp>` — build Sekiro fixture, load, assert structure ⚠️ destructive
  - `sekiro-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `sekiro-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `sekiro-import-resigns-userid <srcA> <dstB>` — verify Steam ID re-signing on import ⚠️ destructive
- **Read-only subcommands** (safe to pass real save paths):
  - `ds3-real-save-load <path>` — load real DS3 save, print slot availability; **does not modify file**
  - `ds3-real-save-classify <path>` — verify BND4 magic + slot count; **does not modify file**
  - `ds3-dump-summary <path>` — print summary plaintext offsets for diagnosis; **does not modify file**
  - `ds3-backup-slot <src_save> <slot> <dst_backup>` — backup slot from real save to new file; **does not modify `<src_save>`**
- **Fixture-creation subcommands** (write to path, do NOT delete — safe for inspection):
  - `provision-sl2 <output_path>` — create minimal valid ER save fixture, leave on disk
  - `provision-ds3-sl2 <output_path>` — create minimal valid DS3 save fixture, leave on disk
- **`--selftest` subcommands** (selected; see `praxis_selftest.c` for the full list):
  - `smoke` — basic sanity check, prints `praxis_smoke_ok`
  - `locale-dump` — print all STR_PRAXIS_* string values
  - `profile-roundtrip <ini>` — write/reload profile store, assert byte-equal
  - `profile-load <ini>` — dump store contents
  - `profile-add-game <name> <save_dir> <tree_root> <game_id> <ini>` — create game profile
  - `profile-add-backup <parent_id> <name> <tree_root> <comp> <ini>` — create backup profile
  - `profile-list <ini>` — list all profiles
  - `profile-delete-game <id> <ini>`, `profile-delete-backup <id> <ini>`
  - `restore-auto-detect <backup>` — classify backup type
  - `tree-preserve-selection-walkup <root> <sel> <del>` — walk-up selection logic
  - `tree-cycle-sibling-files-sorted <root> <sort_mode> <sel> <direction> <expected>` — sibling navigation under a save tree sort mode
  - `tree-readonly-toggle <root> <relpath>` — set/clear read-only on a save tree file
  - `tree-readonly-reject-folder <root> <relpath>` — verify folders cannot be toggled read-only through save tree file APIs
  - `watcher-state <root>` — start watcher briefly, verify clean exit
  - `backup-full-with-active <src> <dst>` — backup full save using the active backend, exit 0 on success
  - `backup-replace-selected-readonly <save_dir> <tree_root> <relpath>` — verify Backup & Replace rejects read-only selected files
  - `write-raw-bnd4 <src> <dst>`, `classify <file>` — save format helpers
  - `backend-default-save-dir <game_id>` — call backend's `get_default_save_dir` (1=ER, 2=DS3); exit 0 even when no dir found
  - `char-set-name-profile <save_path>` — round-trip char name set/get via ersave profile
  - `detect-system-language-debug` — print locale detection intermediate state
  - `ersave-null-guards` — verify null-guard behavior in ersave public API
  - `theme-change-classify` — verify `theme_core_is_relevant_setting_change` logic
  - `unique-game-name <ini> <base_name>` — print unique game name for the given base name
  - `ds3-aes-known-vector` — assert AES-128-CBC round-trip matches pre-computed vector
  - `ds3-load-min-fixture <tmp>` — build programmatic DS3 fixture, load it, assert structure ⚠️ destructive
  - `ds3-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `ds3-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `ds3-null-guards` — verify all 6 public ds3save functions reject NULL inputs
  - `ds3-import-resigns-userid <srcA> <dstB>` — verify implicit Steam ID re-signing on cross-account import ⚠️ destructive
  - `ds3-real-save-load <path>` — load real DS3 save, print structure
  - `ds3-real-save-classify <path>` — verify BND4 magic + slot count of real save
  - `ds3-real-save-roundtrip-readonly <path> <tmp_copy>` — copy real save, no-op roundtrip, assert reload OK
  - `ds3-dump-summary <path>` — print ACTIVE_OFFSET / AVAILABLE_OFFSET bytes from decrypted summary for diagnosis
  - `ds3-backup-slot <src_save> <slot> <dst_backup>` — backup one DS3 slot to a `.ds3sm` file using DS3 backend directly
  - `dsr-aes-known-vector` — AES-128-CBC KAT for DSR key
  - `dsr-null-guards` — verify null-guard behavior in dsrsave public API
  - `dsr-load-min-fixture <tmp>` — build DSR fixture, load, assert structure ⚠️ destructive
  - `dsr-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `dsr-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `dsr-cross-account-import <srcA> <dstB>` — verify import works across accounts ⚠️ destructive
  - `dsr-real-save-load <path>` — load real DSR save, print slot availability
  - `dsr-real-save-classify <path>` — verify BND4 magic + slot count of real save
  - `dsr-real-save-roundtrip-readonly <path> <tmp_copy>` — copy real save, no-op roundtrip, assert reload OK
  - `ds2s-aes-known-vector` — AES-128-CBC KAT for DS2S key
  - `ds2s-null-guards` — verify null-guard behavior in ds2save public API
  - `ds2s-load-min-fixture <tmp>` — build DS2S fixture, load, assert structure ⚠️ destructive
  - `ds2s-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `ds2s-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `ds2s-dual-slot-roundtrip <tmp>` — verify dual-slot (part A + part B) serialization ⚠️ destructive
  - `ds2s-import-resigns-userid-text <srcA> <dstB>` — verify text Steam ID re-ownership on import ⚠️ destructive
  - `ds2s-available-slots-by-profile-byte <tmp>` — verify availability via profile int32 flag ⚠️ destructive
  - `ds2s-bnd4-entry-count <tmp>` — assert BND4 entry count == 23 ⚠️ destructive
  - `ds2s-real-save-load <path>` — load real DS2S save, print slot availability
  - `ds2s-real-save-classify <path>` — verify BND4 magic + slot count of real save
  - `ds2s-real-save-roundtrip-readonly <path> <tmp_copy>` — copy real save, no-op roundtrip, assert reload OK
  - `sekiro-null-guards` — verify null-guard behavior in sekirosave public API
  - `sekiro-load-min-fixture <tmp>` — build Sekiro fixture, load, assert structure ⚠️ destructive
  - `sekiro-roundtrip-byte-stable <tmp>` — round-trip a no-op import, assert binary equality ⚠️ destructive
  - `sekiro-active-slot <tmp> <expected_int>` — assert active slot matches ⚠️ destructive
  - `sekiro-import-resigns-userid <srcA> <dstB>` — verify Steam ID re-signing on import ⚠️ destructive
  - `sekiro-real-save-load <path>` — load real Sekiro save, print slot availability
  - `sekiro-real-save-classify <path>` — verify BND4 magic + slot count of real save
  - `sekiro-real-save-roundtrip-readonly <path> <tmp_copy>` — copy real save, no-op roundtrip, assert reload OK
  - `provision-sl2 <output_path>` — create minimal valid ER save fixture and leave on disk
  - `provision-ds3-sl2 <output_path>` — create minimal valid DS3 save fixture and leave on disk

---

## Code Style Guidelines

### Language & Standard
- **Pure C11** (`stdbool.h`, `stdint.h`, fixed-width types).
- The root `CMakeLists.txt` sets `CXX_STANDARD 23`; C code uses whatever the toolchain
  default is (no explicit `CMAKE_C_STANDARD` set at root level).
- All source files are compiled with `/utf-8` (MSVC) to allow UTF-8 literals.

### Includes — Order
Within each `.c` file, includes follow this order (no blank lines between groups unless
already present in the codebase):
1. Own module header (`"config.h"`, `"ersave.h"`, …)
2. Other local headers (`"locale.h"`, `"embedded_face_data.h"`, …)
3. Third-party/dep headers (`<ini.h>`, `<md5.h>`)
4. Standard C headers (`<stdint.h>`, `<wchar.h>`, `<stdio.h>`)
5. Windows SDK headers (`<windows.h>` first, then specific ones like `<shlwapi.h>`)

Headers use `#pragma once` (not include guards).

### Naming Conventions

| Item | Convention | Example |
|------|-----------|---------|
| Functions | `snake_case` | `er_save_data_load`, `handle_splitter_drag` |
| Types / structs | `snake_case_t` | `er_save_data_t`, `config_t` |
| Struct tag | `snake_case_s` | `struct er_save_data_s` | 
| Enum types | `snake_case_e` | `locale_string_index_e` |
| Enum values | `UPPER_SNAKE_CASE` | `STR_APP_TITLE`, `STR_MAX` |
| Macros / `#define` | `UPPER_SNAKE_CASE` | `VERSION_STR`, `SPLITTER_WIDTH` |
| Global variables | `snake_case` | `save_data`, `main_window`, `is_dragging` |
| Local variables | `snake_case` | `bytes_read`, `slot_offset` |
| Constants (`#define`) | `UPPER_SNAKE_CASE` | `DEFAULT_SPLIT_RATIO`, `CONFIG_FILE` |

**Module prefix convention:** public API functions are prefixed with their module:
`er_save_*`, `er_char_data_*`, `er_face_data_*`, `config_*`, `locale_*`.

Static (file-local) helpers have no prefix and are declared `static`.

### Types
- Use `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `int` from `<stdint.h>`.
- Use `bool` from `<stdbool.h>` for boolean returns; `BOOL` only where Win32 API demands it.
- `wchar_t` / `PWSTR` / `LPWSTR` for all user-visible strings (Unicode throughout).
- Struct forward-declarations use opaque typedefs in headers; definition stays in `.c`.

### Formatting
- **4-space indentation** (no tabs).
- Opening brace `{` on the **same line** as the control statement for `if`, `for`, `while`,
  `switch`, and function definitions.
- One blank line between top-level function definitions.
- Short one-liner static functions may be written on a single line when it aids readability
  (`validate_face_data` example in `ersave.c`).
- `switch` cases are indented one level; `case` blocks with local variables use `{ }`.
- No trailing whitespace; files end with a newline.

### Comments
- Block comments use `/* … */` (C-style). Single-line `//` comments are acceptable but
  `/* */` is preferred in this codebase.
- Every public function in headers is documented with a Doxygen-style block:
  ```c
  /**
   * @brief Short description
   * @param name Description
   * @return Description
   */
  ```
- Implementation files begin with a `@file` / `@brief` / `@details` Doxygen block.
- Inline comments explain non-obvious logic; struct members have trailing comments.

### Error Handling
- Functions that can fail return `bool` (success) or a pointer (NULL on failure).
- **Early-return pattern** for error paths — check, clean up (free/close handles), and
  `return NULL` / `return false` immediately. Do not use goto.
- Windows HANDLEs are always closed before returning from any error path.
- `LocalAlloc` / `LocalFree` is used for heap allocation (no `malloc`/`free`).
- Win32 HRESULT values are checked with `SUCCEEDED(hr)`.

### Memory Management
- Use `LocalAlloc(LMEM_FIXED, size)` to allocate; `LocalFree(ptr)` to free.
- `ZeroMemory` / `CopyMemory` (Windows macros) for zeroing/copying buffers.
- `lstrcpyW` / `lstrcpynW` for wide-string copying.
- Always free on every error path before the corresponding `return NULL`.
- Every `CreateFileW` must have a matching `CloseHandle` (including error paths).

### Windows API Patterns
- All string literals in the UI use `L"…"` wide-string literals.
- Message-box calls use `locale_str(STR_…)` for the text (never hard-coded strings).
- COM objects (IFileDialog, IShellItem) are released via `->lpVtbl->Release()`.
- UI controls are created with `CreateWindowW` / `CreateWindowExW`; fonts are set with
  `SendMessage(hwnd, WM_SETFONT, …)`.
- Window procedures return `0` when a message is handled; fall through to
  `DefWindowProcW` for unhandled messages.
- `BeginDeferWindowPos` / `DeferWindowPos` / `EndDeferWindowPos` for bulk layout.
- **Theme handling** — every window proc and dialog proc must handle these three messages:
  - `WM_SETTINGCHANGE` → call `theme_core_on_setting_change(wp, lp)`; if it returns `true`, re-apply theme.
  - `WM_SYSCOLORCHANGE` → call `theme_core_on_syscolor_change()`; if it returns `true`, re-apply theme.
  - `WM_THEMECHANGED` → re-apply theme unconditionally.
  - Main windows re-apply with `theme_core_apply_to_window_and_children(hwnd)`; modal dialogs use
    `theme_apply_to_window(hwnd)` (ERSaveManager) or `praxis_theme_apply_to_window(hwnd)` (Praxis).

### CMake Style
- `if () / endif ()` with a space before `()`.
- `cmake_parse_arguments` used in every macro; options in UPPER_CASE.
- Generator expressions (`$<…>`) used for per-config/per-compiler flags.
- No `add_definitions()` — use `target_compile_definitions()` with `PRIVATE`.

---

## Key Domain Notes

- Save file format is **BND4** (FromSoftware container). Magic `"BND4"` at offset 0.
- Character slots 0–9; face slots 0–14; all slots checked for availability before access.
- MD5 checksums are written to the first 16 bytes of each slot before the slot data.
- Steam IDs are 64-bit integers stored as `uint64_t`; folder names are decimal Steam IDs.
- `UNICODE` / `_UNICODE` preprocessor macros are defined — all Win32 calls use the `W`
  suffix variant.
- Locale strings are indexed via `locale_string_index_t`; always use `locale_str(STR_…)`
  rather than hard-coded text.
