/**
 * @file profile_store.c
 * @brief Multi-profile system CRUD for Praxis: game profiles, backup profiles, and store.
 * @details Implements all profile_store_t lifecycle and CRUD helpers.
 *          INI persistence (load/save) lives in profile_store_io.c.
 */

#include "profile_store.h"

#include <windows.h>
#include <shlobj.h>

/* ==== Init ==== */

void profile_store_init(profile_store_t *store) {
    if (store == NULL) {
        return;
    }
    ZeroMemory(store, sizeof(*store));
    store->next_game_id = 1;
    store->next_backup_id = 1;
}

/* ==== Accessors ==== */

const game_profile_t *profile_store_get_active_game(const profile_store_t *store) {
    if (store == NULL || store->active_game_id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == store->active_game_id) {
            return &store->games[i];
        }
    }
    return NULL;
}

const backup_profile_t *profile_store_get_active_backup(const profile_store_t *store) {
    if (store == NULL || store->active_backup_id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].id == store->active_backup_id) {
            return &store->backups[i];
        }
    }
    return NULL;
}

const game_profile_t *profile_store_find_game_by_id(const profile_store_t *store, int id) {
    if (store == NULL || id <= 0) {
        return NULL;
    }
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == id) {
            return &store->games[i];
        }
    }
    return NULL;
}

bool profile_store_resolve_backup_root(const profile_store_t *store,
                                       int backup_id,
                                       wchar_t *out,
                                       size_t out_chars) {
    const backup_profile_t *bp = NULL;
    const game_profile_t *gp = NULL;

    if (store == NULL || out == NULL || out_chars == 0 || backup_id <= 0) {
        return false;
    }

    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].id == backup_id) {
            bp = &store->backups[i];
            break;
        }
    }
    if (bp == NULL) {
        return false;
    }

    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == bp->parent_game_id) {
            gp = &store->games[i];
            break;
        }
    }
    if (gp == NULL) {
        return false;
    }

    return _snwprintf_s(out, out_chars, _TRUNCATE, L"%ls\\%ls",
                        gp->tree_root, bp->name) >= 0;
}

/* ==== Game profile CRUD ==== */

int profile_store_add_game(profile_store_t *store, const game_profile_t *gp) {
    if (store == NULL || gp == NULL) {
        return 0;
    }
    if (store->game_count >= MAX_GAME_PROFILES) {
        return 0;
    }
    /* Validate required fields. */
    if (gp->name[0] == L'\0' || gp->tree_root[0] == L'\0') {
        return 0;
    }

    /* Assign a new sequential ID. */
    int id = store->next_game_id++;
    store->games[store->game_count] = *gp;
    store->games[store->game_count].id = id;
    store->game_count++;

    /* Create tree_root directory if it does not already exist. */
    SHCreateDirectoryExW(NULL, gp->tree_root, NULL);
    /* Ignore return value — ERROR_ALREADY_EXISTS is acceptable. */

    /* Auto-create a Main backup profile for this game. */
    backup_profile_t main_bp;
    wchar_t main_dir[MAX_PATH];

    ZeroMemory(&main_bp, sizeof(main_bp));
    main_bp.parent_game_id = id;
    lstrcpyW(main_bp.name, L"Main");
    main_bp.compression_level = COMP_LEVEL_MEDIUM;

    /* Inline-add the backup, bypassing parent validation (parent was just added). */
    int backup_id = store->next_backup_id++;
    if (store->backup_count < MAX_BACKUP_PROFILES) {
        main_bp.id = backup_id;
        store->backups[store->backup_count++] = main_bp;
        if (profile_store_resolve_backup_root(store, main_bp.id, main_dir, MAX_PATH)) {
            SHCreateDirectoryExW(NULL, main_dir, NULL);
        }
    }

    /* Set both new profiles as active. */
    store->active_game_id = id;
    store->active_backup_id = backup_id;

    return id;
}

bool profile_store_update_game(profile_store_t *store, int id, const game_profile_t *gp) {
    if (store == NULL || gp == NULL || id <= 0) {
        return false;
    }
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == id) {
            store->games[i] = *gp;
            store->games[i].id = id; /* preserve the original ID */
            return true;
        }
    }
    return false;
}

bool profile_store_delete_game(profile_store_t *store, int id) {
    if (store == NULL || id <= 0) {
        return false;
    }

    /* Find the game entry. */
    size_t idx = store->game_count;
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == id) {
            idx = i;
            break;
        }
    }
    if (idx == store->game_count) {
        return false;
    }

    /* Compact the game array. */
    for (size_t i = idx; i + 1 < store->game_count; i++) {
        store->games[i] = store->games[i + 1];
    }
    store->game_count--;

    /* Remove all backup profiles whose parent is the deleted game. */
    size_t j = 0;
    while (j < store->backup_count) {
        if (store->backups[j].parent_game_id == id) {
            int removed_id = store->backups[j].id;
            for (size_t k = j; k + 1 < store->backup_count; k++) {
                store->backups[k] = store->backups[k + 1];
            }
            store->backup_count--;
            /* Clear active backup if it was just removed. */
            if (store->active_backup_id == removed_id) {
                store->active_backup_id = store->backup_count > 0 ? store->backups[0].id : 0;
            }
        } else {
            j++;
        }
    }

    /* Clear active game ID if it was the deleted game. */
    if (store->active_game_id == id) {
        store->active_game_id = store->game_count > 0 ? store->games[0].id : 0;
    }

    return true;
}

/* ==== Backup profile CRUD ==== */

int profile_store_add_backup(profile_store_t *store, const backup_profile_t *bp) {
    if (store == NULL || bp == NULL) {
        return 0;
    }
    if (store->backup_count >= MAX_BACKUP_PROFILES) {
        return 0;
    }
    /* Validate required fields. */
    if (bp->name[0] == L'\0') {
        return 0;
    }
    /* Validate that the parent game profile exists. */
    bool parent_found = false;
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == bp->parent_game_id) {
            parent_found = true;
            break;
        }
    }
    if (!parent_found) {
        return 0;
    }

    int id = store->next_backup_id++;
    wchar_t computed_dir[MAX_PATH];
    store->backups[store->backup_count] = *bp;
    store->backups[store->backup_count].id = id;
    store->backup_count++;

    if (profile_store_resolve_backup_root(store, id, computed_dir, MAX_PATH)) {
        SHCreateDirectoryExW(NULL, computed_dir, NULL);
    }

    return id;
}

bool profile_store_update_backup(profile_store_t *store, int id, const backup_profile_t *bp) {
    if (store == NULL || bp == NULL || id <= 0) {
        return false;
    }
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].id == id) {
            store->backups[i] = *bp;
            store->backups[i].id = id; /* preserve the original ID */
            return true;
        }
    }
    return false;
}

bool profile_store_delete_backup(profile_store_t *store, int id) {
    if (store == NULL || id <= 0) {
        return false;
    }
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].id == id) {
            for (size_t k = i; k + 1 < store->backup_count; k++) {
                store->backups[k] = store->backups[k + 1];
            }
            store->backup_count--;
            if (store->active_backup_id == id) {
                store->active_backup_id = store->backup_count > 0 ? store->backups[0].id : 0;
            }
            return true;
        }
    }
    return false;
}

/* ==== Active selection ==== */

bool profile_store_set_active_game(profile_store_t *store, int id) {
    if (store == NULL || id <= 0) {
        return false;
    }
    for (size_t i = 0; i < store->game_count; i++) {
        if (store->games[i].id == id) {
            const backup_profile_t *active_backup;
            store->active_game_id = id;
            active_backup = profile_store_get_active_backup(store);
            if (active_backup == NULL || active_backup->parent_game_id != id) {
                store->active_backup_id = 0;
                for (size_t j = 0; j < store->backup_count; j++) {
                    if (store->backups[j].parent_game_id == id) {
                        store->active_backup_id = store->backups[j].id;
                        break;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool profile_store_set_active_backup(profile_store_t *store, int id) {
    if (store == NULL || id <= 0) {
        return false;
    }
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].id == id) {
            store->active_backup_id = id;
            return true;
        }
    }
    return false;
}

/* ==== List helpers ==== */

size_t profile_store_list_backups_for_game(const profile_store_t *store, int game_id,
                                           const backup_profile_t **out, size_t out_cap) {
    if (store == NULL) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 0; i < store->backup_count; i++) {
        if (store->backups[i].parent_game_id == game_id) {
            if (out != NULL && count < out_cap) {
                out[count] = &store->backups[i];
            }
            count++;
        }
    }
    return count;
}

/* Return true when a game profile with the given (case-insensitive) name exists. */
static bool game_name_in_use(const profile_store_t *store, const wchar_t *name) {
    if (store == NULL) return false;
    for (size_t i = 0; i < store->game_count; i++) {
        if (lstrcmpiW(store->games[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

bool profile_store_find_unique_game_name(const profile_store_t *store,
                                         const wchar_t *base_name,
                                         wchar_t *out,
                                         size_t out_chars) {
    if (out == NULL || out_chars == 0) {
        return false;
    }
    out[0] = L'\0';
    if (base_name == NULL || base_name[0] == L'\0') {
        return false;
    }

    /* Try the base name first; fall through to numeric suffixes if taken. */
    if (!game_name_in_use(store, base_name)) {
        size_t base_len = (size_t)lstrlenW(base_name);
        if (base_len >= out_chars) {
            return false;
        }
        lstrcpyW(out, base_name);
        return true;
    }

    /* Append " (N)" with N from 2 upward until we find a free slot. */
    for (int suffix = 2; suffix < 100; suffix++) {
        wchar_t candidate[80];
        int written = _snwprintf_s(candidate, _countof(candidate), _TRUNCATE,
                                   L"%ls (%d)", base_name, suffix);
        if (written < 0) {
            /* Truncated by _snwprintf_s; cannot guarantee uniqueness. */
            return false;
        }
        if (!game_name_in_use(store, candidate)) {
            if ((size_t)lstrlenW(candidate) >= out_chars) {
                return false;
            }
            lstrcpyW(out, candidate);
            return true;
        }
    }

    return false;
}
