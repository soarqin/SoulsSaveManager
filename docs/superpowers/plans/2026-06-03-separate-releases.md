# Separate ERSaveManager and Praxis Releases Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the monolithic release workflow into two independent release pipelines with separate Git tag prefixes (`saveman-v*` and `praxis-v*`), separate CHANGELOGs, and independent GitHub Releases.

**Architecture:** Two standalone workflow files (`release-saveman.yml` and `release-praxis.yml`) replace the single `release.yml`. Each workflow triggers on its own tag prefix, builds only its target executable, packages only its own files, and publishes an independent Release. CHANGELOGs are split into product-specific files at the repo root.

**Tech Stack:** GitHub Actions, CMake (Visual Studio 2022), Bash/PowerShell

---

### Task 1: Create `CHANGELOG-ERSaveManager.md`

**Files:**
- Create: `CHANGELOG-ERSaveManager.md`
- Reference: `CHANGELOG.md` (existing)

- [ ] **Step 1: Write the ERSaveManager CHANGELOG**

Content should contain `[Unreleased]` (with ERSaveManager-related/shared entries from the original `[Unreleased]` and the two ERSaveManager entries from `[1.1.0]`) plus the original `[1.0.0]` section.

```markdown
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed
- `docs/DS3SaveFormatResearch.md` Steam ID width corrected from 16 bytes to 8 bytes (was a documentation error)
- ERSaveManager: Source files relocated to `src/ERSaveManager/` subdirectory (no behavior change)
- Repository: Shared `src/common/` static library extracted (ersave, save_compress, file_dialog, locale_core, config_core)

### Build
- `ersave_common` static library now links `bcrypt` (Windows CNG) PUBLIC for AES-128-CBC used by DS3 save handling

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
```

- [ ] **Step 2: Verify file created correctly**

Run: `head -n 5 CHANGELOG-ERSaveManager.md`
Expected output: `# Changelog` header present.

---

### Task 2: Create `CHANGELOG-Praxis.md`

**Files:**
- Create: `CHANGELOG-Praxis.md`

- [ ] **Step 1: Write the Praxis CHANGELOG**

```markdown
# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-06-03

### Added

- Initial release of Praxis.

[1.0.0]: https://github.com/soarqin/ERSaveManager/releases/tag/praxis-v1.0.0
```

- [ ] **Step 2: Verify file created correctly**

Run: `cat CHANGELOG-Praxis.md`
Expected: Content matches the template above.

---

### Task 3: Delete old `CHANGELOG.md`

**Files:**
- Delete: `CHANGELOG.md`

- [ ] **Step 1: Remove old unified CHANGELOG**

Run: `git rm CHANGELOG.md`

- [ ] **Step 2: Stage new CHANGELOGs**

Run: `git add CHANGELOG-ERSaveManager.md CHANGELOG-Praxis.md`

- [ ] **Step 3: Commit CHANGELOG changes**

Run:
```bash
git commit -m "chore: split CHANGELOG into product-specific files

- CHANGELOG-ERSaveManager.md: retains [1.0.0] and moves shared/ERSaveManager entries from [Unreleased] and [1.1.0] into [Unreleased]
- CHANGELOG-Praxis.md: initial release placeholder for Praxis v1.0.0
- Removes the unified CHANGELOG.md"
```

---

### Task 4: Create `.github/workflows/release-saveman.yml`

**Files:**
- Create: `.github/workflows/release-saveman.yml`
- Reference: `.github/workflows/release.yml` (to be deleted later)

- [ ] **Step 1: Write the ERSaveManager release workflow**

```yaml
name: Release ERSaveManager

on:
  push:
    tags:
      - "saveman-v*"

permissions:
  contents: write

jobs:
  build:
    runs-on: windows-latest

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Get version from tag
        id: version
        shell: bash
        run: |
          VERSION="${GITHUB_REF_NAME#saveman-v}"
          echo "VERSION=$VERSION" >> "$GITHUB_OUTPUT"
          if [[ "$VERSION" == *-* ]]; then
            echo "PRERELEASE=true" >> "$GITHUB_OUTPUT"
          else
            echo "PRERELEASE=false" >> "$GITHUB_OUTPUT"
          fi

      - name: Extract changelog
        id: changelog
        shell: bash
        run: |
          VERSION="${{ steps.version.outputs.VERSION }}"
          {
            echo "BODY<<CHANGELOG_EOF"
            awk -v ver="$VERSION" '
              /^## \[/ { if (found) exit; if (index($0, "[" ver "]")) { found=1; next } }
              found { print }
            ' CHANGELOG-ERSaveManager.md
            echo "CHANGELOG_EOF"
          } >> "$GITHUB_OUTPUT"

      - name: Configure CMake
        run: cmake -S . -B build -G "Visual Studio 17 2022" -T v142 -A x64 -DRELEASE_USE_STATIC_CRT=OFF

      - name: Build
        run: cmake --build build --config MinSizeRel --target saveman

      - name: Package
        shell: bash
        run: |
          mkdir release
          cp build/bin/ERSaveManager.exe release/
          cp README.md LICENSE release/
          cp CHANGELOG-ERSaveManager.md release/CHANGELOG.md

      - name: Create archive
        shell: pwsh
        run: Compress-Archive -Path release/* -DestinationPath "ERSaveManager-${{ steps.version.outputs.VERSION }}.zip"

      - name: Create GitHub Release
        uses: softprops/action-gh-release@v2
        with:
          name: ERSaveManager v${{ steps.version.outputs.VERSION }}
          body: ${{ steps.changelog.outputs.BODY }}
          prerelease: ${{ steps.version.outputs.PRERELEASE }}
          files: ERSaveManager-${{ steps.version.outputs.VERSION }}.zip
```

- [ ] **Step 2: Verify workflow syntax**

Run: `git add .github/workflows/release-saveman.yml`
No syntax errors expected at this stage (full validation happens at push time).

---

### Task 5: Create `.github/workflows/release-praxis.yml`

**Files:**
- Create: `.github/workflows/release-praxis.yml`

- [ ] **Step 1: Write the Praxis release workflow**

```yaml
name: Release Praxis

on:
  push:
    tags:
      - "praxis-v*"

permissions:
  contents: write

jobs:
  build:
    runs-on: windows-latest

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Get version from tag
        id: version
        shell: bash
        run: |
          VERSION="${GITHUB_REF_NAME#praxis-v}"
          echo "VERSION=$VERSION" >> "$GITHUB_OUTPUT"
          if [[ "$VERSION" == *-* ]]; then
            echo "PRERELEASE=true" >> "$GITHUB_OUTPUT"
          else
            echo "PRERELEASE=false" >> "$GITHUB_OUTPUT"
          fi

      - name: Extract changelog
        id: changelog
        shell: bash
        run: |
          VERSION="${{ steps.version.outputs.VERSION }}"
          {
            echo "BODY<<CHANGELOG_EOF"
            awk -v ver="$VERSION" '
              /^## \[/ { if (found) exit; if (index($0, "[" ver "]")) { found=1; next } }
              found { print }
            ' CHANGELOG-Praxis.md
            echo "CHANGELOG_EOF"
          } >> "$GITHUB_OUTPUT"

      - name: Configure CMake
        run: cmake -S . -B build -G "Visual Studio 17 2022" -T v142 -A x64 -DRELEASE_USE_STATIC_CRT=OFF

      - name: Build
        run: cmake --build build --config MinSizeRel --target praxis

      - name: Package
        shell: bash
        run: |
          mkdir release
          cp build/bin/Praxis.exe release/
          cp README.md LICENSE release/
          cp CHANGELOG-Praxis.md release/CHANGELOG.md

      - name: Create archive
        shell: pwsh
        run: Compress-Archive -Path release/* -DestinationPath "Praxis-${{ steps.version.outputs.VERSION }}.zip"

      - name: Create GitHub Release
        uses: softprops/action-gh-release@v2
        with:
          name: Praxis v${{ steps.version.outputs.VERSION }}
          body: ${{ steps.changelog.outputs.BODY }}
          prerelease: ${{ steps.version.outputs.PRERELEASE }}
          files: Praxis-${{ steps.version.outputs.VERSION }}.zip
```

- [ ] **Step 2: Verify workflow syntax**

Run: `git add .github/workflows/release-praxis.yml`

- [ ] **Step 3: Commit new workflow files**

Run:
```bash
git commit -m "ci: add independent release workflows for ERSaveManager and Praxis

- release-saveman.yml: triggers on saveman-v*, builds saveman target only
- release-praxis.yml: triggers on praxis-v*, builds praxis target only
- Each workflow packages only its own exe, README, LICENSE, and product-specific CHANGELOG"
```

---

### Task 6: Delete old `.github/workflows/release.yml`

**Files:**
- Delete: `.github/workflows/release.yml`

- [ ] **Step 1: Remove old unified release workflow**

Run: `git rm .github/workflows/release.yml`

- [ ] **Step 2: Commit deletion**

Run:
```bash
git commit -m "ci: remove monolithic release.yml

Replaced by product-specific release-saveman.yml and release-praxis.yml"
```

---

## Self-Review Checklist

**1. Spec coverage:**
- [x] Two tag prefixes (`saveman-v*`, `praxis-v*`) → Task 4 and Task 5
- [x] Separate CHANGELOG files → Task 1, Task 2, Task 3
- [x] Independent GitHub Releases with distinct names/archives → Task 4 and Task 5
- [x] Only build corresponding target (`saveman` / `praxis`) → Task 4 and Task 5
- [x] Package only corresponding files → Task 4 and Task 5
- [x] Rename CHANGELOG during packaging → Task 4 (`cp CHANGELOG-ERSaveManager.md release/CHANGELOG.md`) and Task 5 (`cp CHANGELOG-Praxis.md release/CHANGELOG.md`)
- [x] Delete old unified files → Task 3 and Task 6

**2. Placeholder scan:**
- [x] No TBD/TODO/fill-in-details found
- [x] Every step contains exact file paths and exact code
- [x] No vague instructions like "add error handling"

**3. Type/Name consistency:**
- [x] `CHANGELOG-ERSaveManager.md` used consistently
- [x] `CHANGELOG-Praxis.md` used consistently
- [x] Tag prefixes match design doc (`saveman-v*`, `praxis-v*`)
- [x] Archive names match design doc (`ERSaveManager-<VERSION>.zip`, `Praxis-<VERSION>.zip`)
- [x] Release names match design doc (`ERSaveManager v<VERSION>`, `Praxis v<VERSION>`)
