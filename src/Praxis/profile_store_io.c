/**
 * @file profile_store_io.c
 * @brief Profile store INI persistence: load and save.
 * @details Implements profile_store_io_load and profile_store_io_save.
 *          Loading uses config_core_parse_ini_ex for multi-section parsing.
 *          Saving writes a complete INI file atomically via a temp file and
 *          MoveFileExW.  Orphan backup profiles (whose parent game no longer
 *          exists) are silently removed during load.
 */

#include "profile_store_io.h"
#include "config.h"
#include "../common/config_core.h"
#include "backend_registry.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <windows.h>
#include <shlwapi.h>

/* Maximum INI file size accepted by profile_store_io_load (256 KiB). */
#define PROFILE_STORE_MAX_BYTES (256u * 1024u)

/* ==== Load: parsing state and callbacks ==== */

/**
 * @brief Parsing context threaded through the section and kv callbacks during load.
 */
typedef struct parse_ctx_s {
    profile_store_t *store;
    int current_section_type;   /* 0=unknown, 1=settings, 2=game_profile, 3=backup_profile */
    int current_section_id;     /* The N parsed from [GameProfile:N] or [BackupProfile:N] */
    game_profile_t   cur_game;  /* Accumulator for the current GameProfile section */
    backup_profile_t cur_backup; /* Accumulator for the current BackupProfile section */
    bool cur_game_uses_legacy_save_dir;
} parse_ctx_t;

static void normalize_loaded_game_profile(game_profile_t *gp, bool legacy_save_dir) {
    const game_backend_t *backend;
    const wchar_t *filename;

    if (gp == NULL || !legacy_save_dir || gp->original_save_dir[0] == L'\0') {
        return;
    }

    backend = backend_registry_get_by_id(gp->game_id);
    filename = (backend && backend->save_filename && backend->save_filename[0])
        ? backend->save_filename : NULL;
    if (filename == NULL) {
        return;
    }

    PathAppendW(gp->original_save_dir, filename);
}

/**
 * @brief Commit the pending profile into the store, then identify the new section.
 */
static void load_section_cb(const char *section, void *user) {
    parse_ctx_t *ctx = (parse_ctx_t *)user;

    /* Commit the previous section's accumulated data before switching. */
    if (ctx->current_section_type == 2 && ctx->current_section_id > 0) {
        if (ctx->store->game_count < MAX_GAME_PROFILES) {
            normalize_loaded_game_profile(&ctx->cur_game, ctx->cur_game_uses_legacy_save_dir);
            ctx->cur_game.id = ctx->current_section_id;
            ctx->store->games[ctx->store->game_count++] = ctx->cur_game;
        }
    } else if (ctx->current_section_type == 3 && ctx->current_section_id > 0) {
        if (ctx->store->backup_count < MAX_BACKUP_PROFILES) {
            ctx->cur_backup.id = ctx->current_section_id;
            ctx->store->backups[ctx->store->backup_count++] = ctx->cur_backup;
        }
    }

    ZeroMemory(&ctx->cur_game, sizeof(ctx->cur_game));
    ZeroMemory(&ctx->cur_backup, sizeof(ctx->cur_backup));
    ctx->cur_game_uses_legacy_save_dir = false;
    ctx->cur_backup.compression_level = COMP_LEVEL_MEDIUM; /* default when key is absent */
    ctx->current_section_id = 0;

    /* Classify the incoming section name. */
    if (strcmp(section, "Settings") == 0) {
        ctx->current_section_type = 1;
    } else if (strncmp(section, "GameProfile:", 12) == 0) {
        ctx->current_section_type = 2;
        ctx->current_section_id = atoi(section + 12);
    } else if (strncmp(section, "BackupProfile:", 14) == 0) {
        ctx->current_section_type = 3;
        ctx->current_section_id = atoi(section + 14);
    } else {
        ctx->current_section_type = 0;
    }
}

/**
 * @brief Dispatch a key=value pair to the correct accumulator.
 */
static void load_kv_cb(const char *key, const char *value, void *user) {
    parse_ctx_t *ctx = (parse_ctx_t *)user;

    if (ctx->current_section_type == 1) {
        /* [Settings] — only profile-store-owned keys. */
        if (strcmp(key, "ActiveGameProfileId") == 0) {
            ctx->store->active_game_id = atoi(value);
        } else if (strcmp(key, "ActiveBackupProfileId") == 0) {
            ctx->store->active_backup_id = atoi(value);
        }
        /* Other Settings keys (TreeRoot, Language, etc.) are owned by praxis_load_config. */
    } else if (ctx->current_section_type == 2) {
        /* [GameProfile:N] */
        if (strcmp(key, "Name") == 0) {
            config_core_store_wide_value(ctx->cur_game.name, 64, value);
        } else if (strcmp(key, "GameId") == 0) {
            ctx->cur_game.game_id = (game_id_t)atoi(value);
        } else if (strcmp(key, "OriginalSaveFile") == 0) {
            config_core_store_wide_value(ctx->cur_game.original_save_dir, MAX_PATH, value);
            ctx->cur_game_uses_legacy_save_dir = false;
        } else if (strcmp(key, "OriginalSaveDir") == 0) {
            config_core_store_wide_value(ctx->cur_game.original_save_dir, MAX_PATH, value);
            ctx->cur_game_uses_legacy_save_dir = true;
        } else if (strcmp(key, "TreeRoot") == 0) {
            config_core_store_wide_value(ctx->cur_game.tree_root, MAX_PATH, value);
        }
    } else if (ctx->current_section_type == 3) {
        /* [BackupProfile:N] */
        if (strcmp(key, "ParentGameId") == 0) {
            ctx->cur_backup.parent_game_id = atoi(value);
        } else if (strcmp(key, "Name") == 0) {
            config_core_store_wide_value(ctx->cur_backup.name, 64, value);
        } else if (strcmp(key, "CompressionLevel") == 0) {
            if (strcmp(value, "none") == 0) {
                ctx->cur_backup.compression_level = COMP_LEVEL_NONE;
            } else if (strcmp(value, "medium") == 0) {
                ctx->cur_backup.compression_level = COMP_LEVEL_MEDIUM;
            } else if (strcmp(value, "high") == 0) {
                ctx->cur_backup.compression_level = COMP_LEVEL_HIGH;
            } else {
                ctx->cur_backup.compression_level = COMP_LEVEL_LOW; /* default */
            }
        }
    }
}

/* ==== Load ==== */

bool profile_store_io_load(profile_store_t *out_store, const wchar_t *ini_path) {
    if (out_store == NULL || ini_path == NULL) {
        return false;
    }

    profile_store_init(out_store);

    HANDLE fh = CreateFileW(ini_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        return true; /* File not found is OK (fresh install, empty store). */
    }

    DWORD file_size = GetFileSize(fh, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size == 0 || file_size > PROFILE_STORE_MAX_BYTES) {
        CloseHandle(fh);
        return true; /* Empty or oversized file: treat as fresh store. */
    }

    char *buf = (char *)LocalAlloc(LMEM_FIXED, file_size + 1u);
    if (buf == NULL) {
        CloseHandle(fh);
        return false;
    }

    DWORD bytes_read = 0;
    if (!ReadFile(fh, buf, file_size, &bytes_read, NULL)) {
        LocalFree(buf);
        CloseHandle(fh);
        return false;
    }
    CloseHandle(fh);
    buf[bytes_read] = '\0';

    parse_ctx_t ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.store = out_store;

    config_core_parse_ini_ex(buf, (size_t)bytes_read, load_section_cb, load_kv_cb, &ctx);

    /* Flush the last pending section (no following section header triggers the commit). */
    load_section_cb("", &ctx);

    LocalFree(buf);

    /* Orphan cleanup: remove backups whose parent game no longer exists. */
    size_t i = 0;
    while (i < out_store->backup_count) {
        bool found = false;
        for (size_t j = 0; j < out_store->game_count; j++) {
            if (out_store->games[j].id == out_store->backups[i].parent_game_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            wchar_t debug_msg[128];
            _snwprintf_s(debug_msg, 128, _TRUNCATE,
                L"praxis: orphan BackupProfile:%d parent=%d (skipped)\n",
                out_store->backups[i].id, out_store->backups[i].parent_game_id);
            OutputDebugStringW(debug_msg);
            for (size_t k = i; k + 1 < out_store->backup_count; k++) {
                out_store->backups[k] = out_store->backups[k + 1];
            }
            out_store->backup_count--;
        } else {
            i++;
        }
    }

    /* Active game ID fallback: if the active game is gone, pick the first available. */
    if (out_store->active_game_id != 0) {
        bool found = false;
        for (size_t j = 0; j < out_store->game_count; j++) {
            if (out_store->games[j].id == out_store->active_game_id) {
                found = true;
                break;
            }
        }
        if (!found) {
            out_store->active_game_id = out_store->game_count > 0 ? out_store->games[0].id : 0;
        }
    }

    /* Active backup ID fallback: if the active backup is gone or belongs to another game, pick the first active-game backup. */
    if (out_store->active_backup_id != 0) {
        bool found = false;
        for (size_t j = 0; j < out_store->backup_count; j++) {
            if (out_store->backups[j].id == out_store->active_backup_id &&
                (out_store->active_game_id == 0 || out_store->backups[j].parent_game_id == out_store->active_game_id)) {
                found = true;
                break;
            }
        }
        if (!found) {
            out_store->active_backup_id = 0;
            for (size_t j = 0; j < out_store->backup_count; j++) {
                if (out_store->active_game_id == 0 || out_store->backups[j].parent_game_id == out_store->active_game_id) {
                    out_store->active_backup_id = out_store->backups[j].id;
                    break;
                }
            }
        }
    }

    /* Recompute next_game_id as max(all game IDs) + 1. */
    int max_game_id = 0;
    for (size_t j = 0; j < out_store->game_count; j++) {
        if (out_store->games[j].id > max_game_id) {
            max_game_id = out_store->games[j].id;
        }
    }
    out_store->next_game_id = max_game_id + 1;

    /* Recompute next_backup_id as max(all backup IDs) + 1. */
    int max_backup_id = 0;
    for (size_t j = 0; j < out_store->backup_count; j++) {
        if (out_store->backups[j].id > max_backup_id) {
            max_backup_id = out_store->backups[j].id;
        }
    }
    out_store->next_backup_id = max_backup_id + 1;

    return true;
}

/* ==== Save ==== */

bool profile_store_io_save(const profile_store_t *store, const wchar_t *ini_path) {
    if (store == NULL || ini_path == NULL) {
        return false;
    }

    /* Build the temporary file path by appending ".tmp". */
    wchar_t tmp_path[MAX_PATH];
    _snwprintf(tmp_path, MAX_PATH, L"%ls.tmp", ini_path);
    tmp_path[MAX_PATH - 1] = L'\0';

    /* Determine legacy TreeRoot: prefer active backup's computed root for backward compat. */
    const backup_profile_t *active_bp = profile_store_get_active_backup(store);
    wchar_t legacy_tree_root[MAX_PATH] = {0};

    char tree_root_utf8[MAX_PATH * 4] = {0};
    if (active_bp != NULL &&
        profile_store_resolve_backup_root(store, active_bp->id, legacy_tree_root, MAX_PATH)) {
        WideCharToMultiByte(CP_UTF8, 0, legacy_tree_root, -1,
                            tree_root_utf8, (int)sizeof(tree_root_utf8), NULL, NULL);
    } else {
        WideCharToMultiByte(CP_UTF8, 0, praxis_config.tree_root, -1,
                            tree_root_utf8, (int)sizeof(tree_root_utf8), NULL, NULL);
    }

    /* Map active backup compression level to a string for the legacy
     * [Settings] CompressionLevel field. praxis_load_config still accepts
     * the legacy integer form (1/5/9) so older INI files keep loading. */
    const char *legacy_comp_str;
    switch (active_bp ? active_bp->compression_level : COMP_LEVEL_MEDIUM) {
    case COMP_LEVEL_NONE:   legacy_comp_str = "none";   break;
    case COMP_LEVEL_HIGH:   legacy_comp_str = "high";   break;
    case COMP_LEVEL_LOW:    legacy_comp_str = "low";    break;
    default:                legacy_comp_str = "medium"; break;
    }

    /* Convert hotkey strings from wide to UTF-8. */
    char hotkey_bf[32 * 4]  = {0};
    char hotkey_bs[32 * 4]  = {0};
    char hotkey_r[32 * 4]   = {0};
    char hotkey_ur[32 * 4]  = {0};
    char hotkey_br[32 * 4]  = {0};
    char hotkey_prev[32 * 4] = {0};
    char hotkey_next[32 * 4] = {0};
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_backup_full, -1,
                        hotkey_bf, (int)sizeof(hotkey_bf), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_backup_slot, -1,
                        hotkey_bs, (int)sizeof(hotkey_bs), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_restore, -1,
                        hotkey_r, (int)sizeof(hotkey_r), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_undo_restore, -1,
                        hotkey_ur, (int)sizeof(hotkey_ur), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_backup_replace, -1,
                        hotkey_br, (int)sizeof(hotkey_br), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_previous_save, -1,
                        hotkey_prev, (int)sizeof(hotkey_prev), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, praxis_config.hotkey_next_save, -1,
                        hotkey_next, (int)sizeof(hotkey_next), NULL, NULL);

    config_core_buf_t buf;
    config_core_buf_init(&buf);

    /* [Settings] section: all praxis_config fields plus profile store selections. */
    config_core_buf_append(&buf, "[Settings]\r\n");
    config_core_buf_append(&buf, "TreeRoot=%s\r\n", tree_root_utf8);
    config_core_buf_append(&buf, "Language=%d\r\n", praxis_config.language);
    config_core_buf_append(&buf, "WindowX=%d\r\n", praxis_config.window_x);
    config_core_buf_append(&buf, "WindowY=%d\r\n", praxis_config.window_y);
    config_core_buf_append(&buf, "WindowWidth=%d\r\n", praxis_config.window_width);
    config_core_buf_append(&buf, "WindowHeight=%d\r\n", praxis_config.window_height);
    config_core_buf_append(&buf, "CompressionLevel=%s\r\n", legacy_comp_str);
    config_core_buf_append(&buf, "RingSize=%d\r\n", praxis_config.ring_size);
    config_core_buf_append(&buf, "HotkeyBackupFull=%s\r\n", hotkey_bf);
    config_core_buf_append(&buf, "HotkeyBackupSlot=%s\r\n", hotkey_bs);
    config_core_buf_append(&buf, "HotkeyRestore=%s\r\n", hotkey_r);
    config_core_buf_append(&buf, "HotkeyUndoRestore=%s\r\n", hotkey_ur);
    config_core_buf_append(&buf, "HotkeyBackupReplace=%s\r\n", hotkey_br);
    config_core_buf_append(&buf, "HotkeyPreviousSave=%s\r\n", hotkey_prev);
    config_core_buf_append(&buf, "HotkeyNextSave=%s\r\n", hotkey_next);
    config_core_buf_append(&buf, "ActiveGameProfileId=%d\r\n", store->active_game_id);
    config_core_buf_append(&buf, "ActiveBackupProfileId=%d\r\n", store->active_backup_id);
    config_core_buf_append(&buf, "MigrationDismissed=%d\r\n", praxis_config.migration_dismissed);
    config_core_buf_append(&buf, "Theme=%d\r\n", praxis_config.theme);
    config_core_buf_append(&buf, "\r\n");

    /* [GameProfile:N] sections. */
    for (size_t gi = 0; gi < store->game_count; gi++) {
        const game_profile_t *gp = &store->games[gi];
        char name_utf8[64 * 4]          = {0};
        char orig_dir_utf8[MAX_PATH * 4] = {0};
        char game_tree_utf8[MAX_PATH * 4] = {0};

        WideCharToMultiByte(CP_UTF8, 0, gp->name, -1,
                            name_utf8, (int)sizeof(name_utf8), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, gp->original_save_dir, -1,
                            orig_dir_utf8, (int)sizeof(orig_dir_utf8), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, gp->tree_root, -1,
                            game_tree_utf8, (int)sizeof(game_tree_utf8), NULL, NULL);

        config_core_buf_append(&buf, "[GameProfile:%d]\r\n", gp->id);
        config_core_buf_append(&buf, "Name=%s\r\n", name_utf8);
        config_core_buf_append(&buf, "GameId=%d\r\n", (int)gp->game_id);
        config_core_buf_append(&buf, "OriginalSaveFile=%s\r\n", orig_dir_utf8);
        config_core_buf_append(&buf, "TreeRoot=%s\r\n", game_tree_utf8);
        config_core_buf_append(&buf, "\r\n");
    }

    /* [BackupProfile:N] sections. */
    for (size_t bi = 0; bi < store->backup_count; bi++) {
        const backup_profile_t *bp = &store->backups[bi];
        char name_utf8[64 * 4] = {0};
        const char *comp_str;

        WideCharToMultiByte(CP_UTF8, 0, bp->name, -1,
                            name_utf8, (int)sizeof(name_utf8), NULL, NULL);

        switch (bp->compression_level) {
        case COMP_LEVEL_NONE:   comp_str = "none";   break;
        case COMP_LEVEL_MEDIUM: comp_str = "medium";  break;
        case COMP_LEVEL_HIGH:   comp_str = "high";   break;
        default:                comp_str = "low";    break;
        }

        config_core_buf_append(&buf, "[BackupProfile:%d]\r\n", bp->id);
        config_core_buf_append(&buf, "ParentGameId=%d\r\n", bp->parent_game_id);
        config_core_buf_append(&buf, "Name=%s\r\n", name_utf8);
        config_core_buf_append(&buf, "CompressionLevel=%s\r\n", comp_str);
        config_core_buf_append(&buf, "\r\n");
    }

    /* Write to the temp file. */
    if (!config_core_buf_write_file(&buf, tmp_path)) {
        config_core_buf_free(&buf);
        return false;
    }
    config_core_buf_free(&buf);

    /* Atomic replace: rename temp → final path. */
    if (!MoveFileExW(tmp_path, ini_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path);
        return false;
    }

    return true;
}
