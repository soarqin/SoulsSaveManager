/**
 * @file ds3_backend.c
 * @brief Dark Souls III game backend implementation.
 * @details Implements full-save and slot-level save operations for Dark Souls III.
 */

#include "../game_backend.h"

#include "ds3save.h"
#include "save_compress.h"

#include <stdint.h>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Resolve the first %APPDATA%\DarkSoulsIII\<hex_userid>\ that contains DS30000.sl2.
 * Used as a UI hint for the Add Game dialog (initial folder in the picker).
 * Returns the directory path (no trailing DS30000.sl2). */
static bool ds3_get_default_save_dir(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t ds3_dir[MAX_PATH];
    lstrcpyW(ds3_dir, appdata);
    if (!PathAppendW(ds3_dir, L"DarkSoulsIII")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, ds3_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        bool is_hex = true;
        for (int i = 0; fd.cFileName[i] != L'\0'; i++) {
            wchar_t c = fd.cFileName[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'))) {
                is_hex = false;
                break;
            }
        }
        if (!is_hex) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, ds3_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;

        wchar_t save_check[MAX_PATH];
        lstrcpyW(save_check, candidate);
        if (!PathAppendW(save_check, L"DS30000.sl2")) continue;
        if (GetFileAttributesW(save_check) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* Resolve the first found DS30000.sl2 under %APPDATA%\DarkSoulsIII\<hex_userid>\ */
static bool ds3_resolve_save_path(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t ds3_dir[MAX_PATH];
    lstrcpyW(ds3_dir, appdata);
    if (!PathAppendW(ds3_dir, L"DarkSoulsIII")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, ds3_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        bool is_hex = true;
        for (int i = 0; fd.cFileName[i] != L'\0'; i++) {
            wchar_t c = fd.cFileName[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'))) {
                is_hex = false;
                break;
            }
        }
        if (!is_hex) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, ds3_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;
        if (!PathAppendW(candidate, L"DS30000.sl2")) continue;
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* DS3 save files are AES-128-CBC encrypted at the slot level; encrypted data
 * has near-random entropy and is effectively incompressible. Always write a
 * raw BND4 copy and ignore the requested compression level. The restore path
 * (ds3_restore_full) detects raw vs. compressed via ersm_detect_file_format
 * and handles both formats. */
static bool ds3_backup_full(const wchar_t *src, const wchar_t *dst, int level) {
    (void)level;

    HANDLE file = CreateFileW(src, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD file_size = GetFileSize(file, NULL);
    if (file_size == INVALID_FILE_SIZE || file_size < 4) {
        CloseHandle(file);
        return false;
    }

    uint8_t *buf = LocalAlloc(LMEM_FIXED, file_size);
    if (!buf) {
        CloseHandle(file);
        return false;
    }

    DWORD bytes_read;
    if (!ReadFile(file, buf, file_size, &bytes_read, NULL) || bytes_read != file_size) {
        LocalFree(buf);
        CloseHandle(file);
        return false;
    }

    CloseHandle(file);

    if (buf[0] != 'B' || buf[1] != 'N' || buf[2] != 'D' || buf[3] != '4') {
        LocalFree(buf);
        return false;
    }

    bool ok = ersm_write_raw_bnd4_to_file(dst, buf, file_size);
    LocalFree(buf);
    return ok;
}

static bool ds3_restore_full(const wchar_t *src_backup, const wchar_t *dst_save) {
    ersm_format_t fmt = ersm_detect_file_format(src_backup);

    if (fmt == ERSM_FMT_BND4_RAW) {
        SetFileAttributesW(dst_save, FILE_ATTRIBUTE_NORMAL);
        return CopyFileW(src_backup, dst_save, FALSE) != 0;
    }

    if (fmt == ERSM_FMT_ERSM_CONTAINER) {
        wchar_t temp_dir[MAX_PATH];
        wchar_t temp_path[MAX_PATH];
        lstrcpyW(temp_dir, dst_save);
        if (!PathRemoveFileSpecW(temp_dir)) return false;
        if (!GetTempFileNameW(temp_dir, L"pxr", 0, temp_path)) return false;

        size_t buf_size = 0;
        uint8_t data_type = 0;
        uint8_t *buf = ersm_decompress_from_file(src_backup, &buf_size, &data_type);
        if (!buf) return false;
        if (data_type != ERSM_TYPE_FULL_SAVE ||
            buf_size < 4 ||
            RtlCompareMemory(buf, "BND4", 4) != 4) {
            LocalFree(buf);
            DeleteFileW(temp_path);
            return false;
        }

        HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) {
            LocalFree(buf);
            DeleteFileW(temp_path);
            return false;
        }

        DWORD written;
        bool ok = WriteFile(file, buf, (DWORD)buf_size, &written, NULL) && written == (DWORD)buf_size;
        CloseHandle(file);
        LocalFree(buf);
        if (!ok) {
            DeleteFileW(temp_path);
            return false;
        }

        SetFileAttributesW(dst_save, FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(temp_path, dst_save, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp_path);
            return false;
        }

        return true;
    }

    return false;
}

static bool ds3_get_active_slot(const wchar_t *save_path, int *out_slot) {
    ds3_save_data_t *save = ds3_save_data_load(save_path);
    bool ok;

    if (!save) return false;

    ok = ds3_save_get_active_slot(save, out_slot);
    ds3_save_data_free(save);
    return ok;
}

static bool ds3_backup_slot(const wchar_t *src_save, int slot, const wchar_t *dst_backup, int level) {
    ds3_save_data_t *save = ds3_save_data_load(src_save);
    const ds3_char_data_t *char_data;
    uint8_t *buf;
    bool ok;

    if (!save) return false;

    char_data = ds3_char_data_ref(save, slot);
    if (!char_data) {
        ds3_save_data_free(save);
        return false;
    }

    /* this value MUST match ds3_char_data_serialize's expected out_size */
    buf = LocalAlloc(LMEM_FIXED, DS3_CHAR_DATA_SERIALIZED_SIZE);
    if (!buf) {
        ds3_save_data_free(save);
        return false;
    }

    ok = ds3_char_data_serialize(char_data, buf, DS3_CHAR_DATA_SERIALIZED_SIZE);
    ds3_save_data_free(save);
    if (!ok) {
        LocalFree(buf);
        return false;
    }

    ok = ersm_compress_to_file(dst_backup, buf, DS3_CHAR_DATA_SERIALIZED_SIZE, ERSM_TYPE_CHAR_SLOT, level);
    LocalFree(buf);
    return ok;
}

static bool ds3_restore_slot(const wchar_t *src_backup, const wchar_t *dst_save, int slot) {
    uint8_t *buf;
    size_t buf_size = 0;
    uint8_t data_type = 0;
    ds3_save_data_t *save;
    bool ok;

    buf = ersm_decompress_from_file(src_backup, &buf_size, &data_type);
    if (!buf) return false;

    if (data_type != ERSM_TYPE_CHAR_SLOT || buf_size != DS3_CHAR_DATA_SERIALIZED_SIZE) {
        LocalFree(buf);
        return false;
    }

    save = ds3_save_data_load(dst_save);
    if (!save) {
        LocalFree(buf);
        return false;
    }

    ok = ds3_char_data_import_raw(save, slot, buf);
    LocalFree(buf);
    ds3_save_data_free(save);
    return ok;
}

const game_backend_t ds3_backend = {
    .id = GAME_ID_DARK_SOULS_3,
    .display_name = L"Dark Souls III",
    .backup_extension = L".ds3sm",
    .save_filename = L"DS30000.sl2",
    .needs_game_restart = false,
    .full_save_skip_compression = true,
    .resolve_save_path = ds3_resolve_save_path,
    .backup_full = ds3_backup_full,
    .restore_full = ds3_restore_full,
    .get_active_slot = ds3_get_active_slot,
    .backup_slot = ds3_backup_slot,
    .restore_slot = ds3_restore_slot,
    .get_default_save_dir = ds3_get_default_save_dir,
};
