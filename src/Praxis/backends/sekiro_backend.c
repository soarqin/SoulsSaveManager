/**
 * @file sekiro_backend.c
 * @brief Sekiro: Shadows Die Twice game backend implementation.
 * @details Implements the game_backend_t vtable for Sekiro.
 *          Uses ER backend as template for backup/restore (both unencrypted/compressible).
 *          Key details:
 *          (1) Save path: %APPDATA%\Sekiro\<decimal_steamid>\S0000.sl2
 *          (2) Folder validation: decimal digits, length <= 20 (max uint64)
 *          (3) Uses CSIDL_APPDATA
 *          (4) full_save_skip_compression = false (unencrypted, LZMA-compressible)
 *          (5) backup_full uses ersm_compress_to_file (NOT raw-copy like DS3)
 */

#include "../game_backend.h"

#include "sekirosave.h"
#include "save_compress.h"

#include <stdint.h>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Validate a folder name as a decimal Steam ID: all digits, length <= 20
 * (uint64 max = 18446744073709551615 → 20 digits). */
static bool is_decimal_steamid(const wchar_t *name) {
    int len = 0;
    for (int i = 0; name[i] != L'\0'; i++) {
        if (name[i] < L'0' || name[i] > L'9') return false;
        len++;
        if (len > 20) return false;
    }
    return len > 0;
}

/* Resolve the first %APPDATA%\Sekiro\<SteamID>\ that contains S0000.sl2.
 * Used as a UI hint for the Add Game dialog (initial folder in the picker).
 * Returns the directory path (no trailing S0000.sl2). */
static bool sekiro_get_default_save_dir(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t sekiro_dir[MAX_PATH];
    lstrcpyW(sekiro_dir, appdata);
    if (!PathAppendW(sekiro_dir, L"Sekiro")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, sekiro_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (!is_decimal_steamid(fd.cFileName)) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, sekiro_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;

        wchar_t save_check[MAX_PATH];
        lstrcpyW(save_check, candidate);
        if (!PathAppendW(save_check, L"S0000.sl2")) continue;
        if (GetFileAttributesW(save_check) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* Resolve the first found S0000.sl2 under %APPDATA%\Sekiro\<SteamID>\ */
static bool sekiro_resolve_save_path(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t sekiro_dir[MAX_PATH];
    lstrcpyW(sekiro_dir, appdata);
    if (!PathAppendW(sekiro_dir, L"Sekiro")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, sekiro_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (!is_decimal_steamid(fd.cFileName)) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, sekiro_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;
        if (!PathAppendW(candidate, L"S0000.sl2")) continue;
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* Sekiro save files are unencrypted BND4 containers (same as Elden Ring), so
 * they compress well with LZMA. Use ersm_compress_to_file for backup_full. */
static bool sekiro_backup_full(const wchar_t *src, const wchar_t *dst, int level) {
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

    bool ok = ersm_compress_to_file(dst, buf, file_size, ERSM_TYPE_FULL_SAVE, level);
    LocalFree(buf);
    return ok;
}

static bool sekiro_restore_full(const wchar_t *src_backup, const wchar_t *dst_save) {
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

static bool sekiro_get_active_slot(const wchar_t *save_path, int *out_slot) {
    sekiro_save_data_t *save = NULL;
    bool ok;

    if (!sekiro_save_data_load(save_path, &save)) return false;

    ok = sekiro_save_get_active_slot(save, out_slot);
    sekiro_save_data_free(save);
    return ok;
}

static bool sekiro_backup_slot(const wchar_t *src_save, int slot, const wchar_t *dst_backup, int level) {
    sekiro_save_data_t *save = NULL;
    sekiro_char_data_t *char_data;
    uint8_t *buf;
    bool ok;

    if (!sekiro_save_data_load(src_save, &save)) return false;

    char_data = sekiro_char_data_ref(save, slot);
    if (!char_data) {
        sekiro_save_data_free(save);
        return false;
    }

    buf = LocalAlloc(LMEM_FIXED, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    if (!buf) {
        sekiro_save_data_free(save);
        return false;
    }

    ok = sekiro_char_data_serialize(char_data, buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE);
    sekiro_save_data_free(save);
    if (!ok) {
        LocalFree(buf);
        return false;
    }

    ok = ersm_compress_to_file(dst_backup, buf, SEKIRO_CHAR_DATA_SERIALIZED_SIZE, ERSM_TYPE_CHAR_SLOT, level);
    LocalFree(buf);
    return ok;
}

static bool sekiro_restore_slot(const wchar_t *src_backup, const wchar_t *dst_save, int slot) {
    uint8_t *buf;
    size_t buf_size = 0;
    uint8_t data_type = 0;
    sekiro_save_data_t *save = NULL;
    bool ok;

    buf = ersm_decompress_from_file(src_backup, &buf_size, &data_type);
    if (!buf) return false;

    if (data_type != ERSM_TYPE_CHAR_SLOT || buf_size != SEKIRO_CHAR_DATA_SERIALIZED_SIZE) {
        LocalFree(buf);
        return false;
    }

    if (!sekiro_save_data_load(dst_save, &save)) {
        LocalFree(buf);
        return false;
    }

    ok = sekiro_char_data_import_raw(save, slot, buf, buf_size);
    LocalFree(buf);
    sekiro_save_data_free(save);
    return ok;
}

const game_backend_t sekiro_backend = {
    .id = GAME_ID_SEKIRO,
    .display_name = L"Sekiro: Shadows Die Twice",
    .backup_extension = L".seksave",
    .save_filename = L"S0000.sl2",
    .needs_game_restart = false,
    .full_save_skip_compression = false,
    .resolve_save_path = sekiro_resolve_save_path,
    .backup_full = sekiro_backup_full,
    .restore_full = sekiro_restore_full,
    .get_active_slot = sekiro_get_active_slot,
    .backup_slot = sekiro_backup_slot,
    .restore_slot = sekiro_restore_slot,
    .get_default_save_dir = sekiro_get_default_save_dir,
};
