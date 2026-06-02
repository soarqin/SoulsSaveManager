/**
 * @file praxis_import_executor.c
 * @brief Batch import executor for save files.
 * @details Handles the actual file I/O for importing scanned saves:
 *          - Original format: full saves stay full saves, slot saves stay slots.
 *          - Single slot: full saves have their active slot extracted; slot saves
 *            are imported as-is.
 *          In both cases the output compression follows the active backup profile's
 *          compression_level and the backend's full_save_skip_compression flag.
 */

#include "praxis_import_executor.h"
#include "backend_registry.h"
#include "game_backend.h"
#include "praxis_hotkey_actions.h"
#include "locale.h"

#include <stdint.h>
#include <wchar.h>

#include <windows.h>
#include <shlwapi.h>

/* Resolve the active backend from a profile store. Same logic as
 * get_active_backend_for in praxis_hotkey_actions.c. */
static const game_backend_t *get_active_backend_for(const profile_store_t *store) {
    const game_profile_t *gp = NULL;
    const backup_profile_t *bp = profile_store_get_active_backup(store);

    if (bp) {
        gp = profile_store_find_game_by_id(store, bp->parent_game_id);
    }
    if (!gp) {
        gp = profile_store_get_active_game(store);
    }
    if (gp) {
        const game_backend_t *backend = backend_registry_get_by_id(gp->game_id);
        if (backend) {
            return backend;
        }
    }
    return backend_registry_get_default();
}

/* Resolve the active save file path. Mirrors resolve_save_path_for in
 * praxis_hotkey_actions.c but declared here as static to avoid linker issues. */
static bool resolve_save_path_for(const profile_store_t *store,
                                  wchar_t *out, size_t out_chars) {
    const backup_profile_t *bp = profile_store_get_active_backup(store);
    const game_profile_t *gp = NULL;
    const game_backend_t *backend = get_active_backend_for(store);

    if (bp) {
        gp = profile_store_find_game_by_id(store, bp->parent_game_id);
    }
    if (!gp) {
        gp = profile_store_get_active_game(store);
    }
    if (!out || out_chars == 0 || !bp || !gp || !backend) {
        return false;
    }
    if (gp->original_save_dir[0] != L'\0') {
        lstrcpynW(out, gp->original_save_dir, (int)out_chars);
        return true;
    }
    return backend->resolve_save_path(out, out_chars);
}

static int comp_level_to_lzma(compression_level_t cl) {
    switch (cl) {
    case COMP_LEVEL_HIGH:   return ERSM_LEVEL_MAX;
    case COMP_LEVEL_MEDIUM: return ERSM_LEVEL_NORMAL;
    case COMP_LEVEL_LOW:    return ERSM_LEVEL_FAST;
    case COMP_LEVEL_NONE:   return ERSM_LEVEL_FAST;
    default:                return ERSM_LEVEL_FAST;
    }
}

static void make_import_filename(const wchar_t *base_dir, const wchar_t *prefix,
                                 const wchar_t *ext, wchar_t *out, size_t out_chars) {
    SYSTEMTIME st;
    if (!base_dir || !prefix || !ext || !out || out_chars == 0) {
        return;
    }
    GetLocalTime(&st);
    _snwprintf_s(out, out_chars, _TRUNCATE,
        L"%ls\\%ls_%04d%02d%02d_%02d%02d%02d%ls",
        base_dir, prefix,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, ext);
}

static bool read_file_to_buffer(const wchar_t *path, uint8_t **out_buf, size_t *out_size) {
    HANDLE file;
    DWORD file_size;
    uint8_t *buf;
    DWORD bytes_read = 0;

    file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    file_size = GetFileSize(file, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size < 4) {
        CloseHandle(file);
        return false;
    }
    buf = (uint8_t *)LocalAlloc(LMEM_FIXED, file_size);
    if (!buf) {
        CloseHandle(file);
        return false;
    }
    if (!ReadFile(file, buf, file_size, &bytes_read, NULL) || bytes_read != file_size) {
        LocalFree(buf);
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    *out_buf = buf;
    *out_size = (size_t)bytes_read;
    return true;
}

static bool import_full_save(const wchar_t *src_path, ersm_format_t src_format,
                             const wchar_t *dst_path, compression_level_t cl,
                             bool skip_compression) {
    uint8_t *data = NULL;
    size_t data_size = 0;
    bool ok = false;

    if (src_format == ERSM_FMT_ERSM_CONTAINER) {
        uint8_t data_type = 0;
        data = ersm_decompress_from_file(src_path, &data_size, &data_type);
        if (!data || data_type != ERSM_TYPE_FULL_SAVE) {
            if (data) LocalFree(data);
            return false;
        }
    } else {
        if (!read_file_to_buffer(src_path, &data, &data_size)) {
            return false;
        }
    }

    if (cl == COMP_LEVEL_NONE || skip_compression) {
        ok = ersm_write_raw_bnd4_to_file(dst_path, data, data_size);
    } else {
        ok = ersm_compress_to_file(dst_path, data, data_size, ERSM_TYPE_FULL_SAVE,
                                   comp_level_to_lzma(cl));
    }

    LocalFree(data);
    return ok;
}

static bool import_as_slot(const game_backend_t *backend, const wchar_t *src_path,
                           save_kind_t src_kind, ersm_format_t src_format,
                           const wchar_t *dst_path, int lzma_level) {
    if (src_kind == SAVE_KIND_SLOT) {
        /* Source is already a slot backup: decompress then re-compress to apply
         * the current profile's compression level. */
        size_t data_size = 0;
        uint8_t data_type = 0;
        uint8_t *data = ersm_decompress_from_file(src_path, &data_size, &data_type);
        bool ok = false;
        if (data && data_type == ERSM_TYPE_CHAR_SLOT) {
            ok = ersm_compress_to_file(dst_path, data, data_size, ERSM_TYPE_CHAR_SLOT, lzma_level);
        }
        if (data) LocalFree(data);
        return ok;
    }

    /* Source is a full save: extract the active slot via backend. */
    {
        int slot = -1;
        if (!game_backend_supports_slot_ops(backend)) {
            return false;
        }
        if (!backend->get_active_slot(src_path, &slot)) {
            return false;
        }
        return backend->backup_slot(src_path, slot, dst_path, lzma_level);
    }
}

int praxis_import_execute(HWND hwnd, profile_store_t *store, save_tree_t *save_tree,
                          const import_scan_result_t *results, const bool *selected,
                          size_t count, import_mode_t mode) {
    const backup_profile_t *bp = profile_store_get_active_backup(store);
    const game_backend_t *backend = get_active_backend_for(store);
    compression_level_t cl;
    wchar_t dst_dir[MAX_PATH];
    const wchar_t *ext;
    int success_count = 0;
    size_t i;

    (void)hwnd;

    if (!bp || !backend) {
        return -1;
    }

    cl = bp->compression_level;
    ext = backend->backup_extension;

    if (!save_tree || !save_tree_get_selected_dir(save_tree, dst_dir, MAX_PATH)) {
        if (!profile_store_resolve_backup_root(store, bp->id, dst_dir, MAX_PATH)) {
            return -1;
        }
    }

    for (i = 0; i < count; i++) {
        wchar_t dst_path[MAX_PATH];
        bool ok = false;

        if (!selected[i]) {
            continue;
        }

        make_import_filename(dst_dir, L"imported", ext, dst_path, MAX_PATH);

        if (mode == IMPORT_MODE_ORIGINAL) {
            if (results[i].kind == SAVE_KIND_FULL) {
                ok = import_full_save(results[i].full_path, results[i].format,
                                      dst_path, cl, backend->full_save_skip_compression);
            } else {
                ok = import_as_slot(backend, results[i].full_path, results[i].kind,
                                    results[i].format, dst_path, comp_level_to_lzma(cl));
            }
        } else { /* IMPORT_MODE_SINGLE_SLOT */
            ok = import_as_slot(backend, results[i].full_path, results[i].kind,
                                results[i].format, dst_path, comp_level_to_lzma(cl));
        }

        if (ok) {
            success_count++;
        }
    }

    if (success_count > 0 && save_tree) {
        save_tree_refresh(save_tree);
    }

    return success_count;
}
