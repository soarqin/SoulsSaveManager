/**
 * @file er_backend.c
 * @brief Elden Ring game backend implementation.
 * @details Implements full-save and slot-level save operations for Elden Ring.
 */

#include "../game_backend.h"

#include "ersave.h"
#include "save_compress.h"

#include <stdint.h>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Resolve the first %APPDATA%\EldenRing\<SteamID>\ that contains ER0000.sl2.
 * Used as a UI hint for the Add Game dialog (initial folder in the picker).
 * Returns the directory path (no trailing ER0000.sl2). */
static bool er_get_default_save_dir(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t er_dir[MAX_PATH];
    lstrcpyW(er_dir, appdata);
    if (!PathAppendW(er_dir, L"EldenRing")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, er_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        bool numeric = true;
        for (int i = 0; fd.cFileName[i] != L'\0'; i++) {
            if (fd.cFileName[i] < L'0' || fd.cFileName[i] > L'9') {
                numeric = false;
                break;
            }
        }
        if (!numeric) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, er_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;

        wchar_t save_check[MAX_PATH];
        lstrcpyW(save_check, candidate);
        if (!PathAppendW(save_check, L"ER0000.sl2")) continue;
        if (GetFileAttributesW(save_check) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* Resolve the first found ER0000.sl2 under %APPDATA%\EldenRing\<SteamID>\ */
static bool er_resolve_save_path(wchar_t *out, size_t out_chars) {
    wchar_t appdata[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata))) {
        return false;
    }

    wchar_t er_dir[MAX_PATH];
    lstrcpyW(er_dir, appdata);
    if (!PathAppendW(er_dir, L"EldenRing")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, er_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        bool numeric = true;
        for (int i = 0; fd.cFileName[i] != L'\0'; i++) {
            if (fd.cFileName[i] < L'0' || fd.cFileName[i] > L'9') {
                numeric = false;
                break;
            }
        }
        if (!numeric) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, er_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;
        if (!PathAppendW(candidate, L"ER0000.sl2")) continue;
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

static bool er_backup_full(const wchar_t *src, const wchar_t *dst, int level) {
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

static bool er_restore_full(const wchar_t *src_backup, const wchar_t *dst_save) {
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

static bool er_get_active_slot(const wchar_t *save_path, int *out_slot) {
    er_save_data_t *save = er_save_data_load(save_path);
    bool ok;

    if (!save) return false;

    ok = er_save_get_active_slot(save, out_slot);
    er_save_data_free(save);
    return ok;
}

static bool er_backup_slot(const wchar_t *src_save, int slot, const wchar_t *dst_backup, int level) {
    er_save_data_t *save = er_save_data_load(src_save);
    const er_char_data_t *char_data;
    uint8_t *buf;
    bool ok;

    if (!save) return false;

    char_data = er_char_data_ref(save, slot);
    if (!char_data) {
        er_save_data_free(save);
        return false;
    }

    buf = LocalAlloc(LMEM_FIXED, 0x28024Cu);
    if (!buf) {
        er_save_data_free(save);
        return false;
    }

    ok = er_char_data_serialize(char_data, buf, 0x28024Cu);
    er_save_data_free(save);
    if (!ok) {
        LocalFree(buf);
        return false;
    }

    ok = ersm_compress_to_file(dst_backup, buf, 0x28024Cu, ERSM_TYPE_CHAR_SLOT, level);
    LocalFree(buf);
    return ok;
}

static bool er_restore_slot(const wchar_t *src_backup, const wchar_t *dst_save, int slot) {
    uint8_t *buf;
    size_t buf_size = 0;
    uint8_t data_type = 0;
    er_save_data_t *save;
    bool ok;

    buf = ersm_decompress_from_file(src_backup, &buf_size, &data_type);
    if (!buf) return false;

    if (data_type != ERSM_TYPE_CHAR_SLOT || buf_size != 0x28024Cu) {
        LocalFree(buf);
        return false;
    }

    save = er_save_data_load(dst_save);
    if (!save) {
        LocalFree(buf);
        return false;
    }

    ok = er_char_data_import_raw(save, slot, buf);
    LocalFree(buf);
    er_save_data_free(save);
    return ok;
}

const game_backend_t er_backend = {
    .id = GAME_ID_ELDEN_RING,
    .display_name = L"Elden Ring",
    .backup_extension = L".ersm",
    .save_filename = L"ER0000.sl2",
    .needs_game_restart = false,
    .full_save_skip_compression = false,
    .resolve_save_path = er_resolve_save_path,
    .backup_full = er_backup_full,
    .restore_full = er_restore_full,
    .get_active_slot = er_get_active_slot,
    .backup_slot = er_backup_slot,
    .restore_slot = er_restore_slot,
    .get_default_save_dir = er_get_default_save_dir,
};
