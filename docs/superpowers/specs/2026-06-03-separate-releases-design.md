# Design: Separate ERSaveManager and Praxis Releases

## Goal

Split the monolithic release workflow into two independent release pipelines so that `ERSaveManager` and `Praxis` can be versioned and released separately using distinct Git tag prefixes.

## Background

Currently, a single `.github/workflows/release.yml` listens on `v*` tags, builds both executables, and publishes them together in one archive. Both products share the same `CHANGELOG.md` and version line.

## Proposed Solution: Independent Workflows (方案A)

Create two separate workflow files, each responsible for one product only. This provides clear separation, independent versioning, and straightforward tag-based triggers.

### Tag Rules

| Product        | Tag Prefix    | Example           |
|----------------|---------------|-------------------|
| ERSaveManager  | `saveman-v`   | `saveman-v1.2.0`  |
| Praxis         | `praxis-v`    | `praxis-v1.0.0`   |

Version numbers (the part after the prefix) follow Semantic Versioning and are extracted by stripping the prefix. Prerelease suffixes (e.g., `-beta`) are still supported.

### Workflow Files

#### `.github/workflows/release-saveman.yml`

- **Trigger:** `push` tags matching `saveman-v*`
- **Build target:** `saveman` (`ERSaveManager.exe`)
- **Package contents:**
  - `ERSaveManager.exe`
  - `README.md`
  - `LICENSE`
  - `CHANGELOG.md` (renamed from `CHANGELOG-ERSaveManager.md` during packaging)
- **Archive name:** `ERSaveManager-<VERSION>.zip`
- **Release name:** `ERSaveManager v<VERSION>`
- **Changelog source:** `CHANGELOG-ERSaveManager.md` (kept in repo root)

#### `.github/workflows/release-praxis.yml`

- **Trigger:** `push` tags matching `praxis-v*`
- **Build target:** `praxis` (`Praxis.exe`)
- **Package contents:**
  - `Praxis.exe`
  - `README.md`
  - `LICENSE`
  - `CHANGELOG.md` (renamed from `CHANGELOG-Praxis.md` during packaging)
- **Archive name:** `Praxis-<VERSION>.zip`
- **Release name:** `Praxis v<VERSION>`
- **Changelog source:** `CHANGELOG-Praxis.md` (kept in repo root)

### CHANGELOG Split Strategy

#### `CHANGELOG-ERSaveManager.md` (new file)

- Keep the `[Unreleased]` section, moving into it any ERSaveManager-related entries from versions greater than `1.0.0` (e.g., the `1.1.0` entries about source relocation and shared library extraction).
- Keep the `[1.0.0] - 2026-04-04` section intact (it documents the ERSaveManager initial release).
- Remove all Praxis-specific entries.

#### `CHANGELOG-Praxis.md` (new file)

- Contains a single `[1.0.0] - 2026-06-03` section marking the initial release.
- No historical update records are kept (per user request).

#### `CHANGELOG.md` (existing file)

- Replaced by `CHANGELOG-ERSaveManager.md` and `CHANGELOG-Praxis.md`.
- Deleted from the repository after the split.

### Version Extraction Logic

```bash
# ERSaveManager
VERSION="${GITHUB_REF_NAME#saveman-v}"

# Praxis
VERSION="${GITHUB_REF_NAME#praxis-v}"
```

Prerelease detection remains unchanged:
```bash
if [[ "$VERSION" == *-* ]]; then
  PRERELEASE=true
else
  PRERELEASE=false
fi
```

### File Change Summary

| Action | File |
|--------|------|
| Delete | `.github/workflows/release.yml` |
| Create | `.github/workflows/release-saveman.yml` |
| Create | `.github/workflows/release-praxis.yml` |
| Delete | `CHANGELOG.md` |
| Create | `CHANGELOG-ERSaveManager.md` |
| Create | `CHANGELOG-Praxis.md` |

## Error Handling

- If a tag does not match either prefix, no workflow runs (by design).
- Changelog extraction uses `awk` with the same pattern as before; if the version section is missing, the release body will be empty but the release will still be created.

## Future Considerations

- If a shared component changes and both products need a new release, two tags must be pushed (`saveman-vX.Y.Z` and `praxis-vX.Y.Z`).
- The `docs/DS3SaveFormatResearch.md` and other shared docs remain in the repo and are included in both releases via `README.md` reference.
