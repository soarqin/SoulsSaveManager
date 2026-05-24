/**
 * @file dsr_backend.c
 * @brief Dark Souls: Remastered game backend implementation.
 * @details Implements the game_backend_t vtable for DSR.
 *          Key differences from DS3 backend:
 *          (1) Save path: Documents\NBGI\DARK SOULS REMASTERED\<decimal_lower32>\DRAKS0005.sl2
 *          (2) Folder validation: decimal digits, length <= 10 (NOT hex like DS3)
 *          (3) Uses CSIDL_PERSONAL (Documents), NOT CSIDL_APPDATA
 *          (4) full_save_skip_compression = true (encrypted save)
 */

#include "../game_backend.h"

#include "dsrsave.h"
#include "save_compress.h"

#include <stdint.h>

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Validate that a folder name is the decimal lower-32-bit Steam ID
 * representation used by DSR (e.g. "42582109").
 * Accepts: 1..10 ASCII decimal digits with no other characters. */
static bool is_decimal_folder(const wchar_t *name) {
    if (!name || name[0] == L'\0') return false;
    int len = 0;
    for (int i = 0; name[i] != L'\0'; i++) {
        wchar_t c = name[i];
        if (c < L'0' || c > L'9') return false;
        len++;
        if (len > 10) return false;
    }
    return true;
}

/* Resolve the first Documents\NBGI\DARK SOULS REMASTERED\<decimal_userid>\
 * that contains DRAKS0005.sl2. Used as a UI hint for the Add Game dialog
 * (initial folder in the picker).
 * Returns the directory path (no trailing DRAKS0005.sl2). */
static bool dsr_get_default_save_dir(wchar_t *out, size_t out_chars) {
    wchar_t documents[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, documents))) {
        return false;
    }

    wchar_t dsr_dir[MAX_PATH];
    lstrcpyW(dsr_dir, documents);
    if (!PathAppendW(dsr_dir, L"NBGI")) return false;
    if (!PathAppendW(dsr_dir, L"DARK SOULS REMASTERED")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, dsr_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (!is_decimal_folder(fd.cFileName)) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, dsr_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;

        wchar_t save_check[MAX_PATH];
        lstrcpyW(save_check, candidate);
        if (!PathAppendW(save_check, L"DRAKS0005.sl2")) continue;
        if (GetFileAttributesW(save_check) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* Resolve the first found DRAKS0005.sl2 under
 * Documents\NBGI\DARK SOULS REMASTERED\<decimal_userid>\ */
static bool dsr_resolve_save_path(wchar_t *out, size_t out_chars) {
    wchar_t documents[MAX_PATH];
    if (!SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, documents))) {
        return false;
    }

    wchar_t dsr_dir[MAX_PATH];
    lstrcpyW(dsr_dir, documents);
    if (!PathAppendW(dsr_dir, L"NBGI")) return false;
    if (!PathAppendW(dsr_dir, L"DARK SOULS REMASTERED")) return false;

    wchar_t search[MAX_PATH];
    lstrcpyW(search, dsr_dir);
    if (!PathAppendW(search, L"*")) return false;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (!is_decimal_folder(fd.cFileName)) continue;

        wchar_t candidate[MAX_PATH];
        lstrcpyW(candidate, dsr_dir);
        if (!PathAppendW(candidate, fd.cFileName)) continue;
        if (!PathAppendW(candidate, L"DRAKS0005.sl2")) continue;
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) continue;
        if ((size_t)lstrlenW(candidate) >= out_chars) continue;

        lstrcpyW(out, candidate);
        found = true;
        break;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return found;
}

/* DSR save files are AES-128-CBC encrypted at the slot level; encrypted data
 * has near-random entropy and is effectively incompressible. Always write a
 * raw BND4 copy and ignore the requested compression level. The restore path
 * (dsr_restore_full) detects raw vs. compressed via ersm_detect_file_format
 * and handles both formats. */
static bool dsr_backup_full(const wchar_t *src, const wchar_t *dst, int level) {
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

static bool dsr_restore_full(const wchar_t *src_backup, const wchar_t *dst_save) {
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

static bool dsr_get_active_slot(const wchar_t *save_path, int *out_slot) {
    dsr_save_data_t *save = NULL;
    bool ok;

    if (!dsr_save_data_load(save_path, &save)) return false;

    ok = dsr_save_get_active_slot(save, out_slot);
    dsr_save_data_free(save);
    return ok;
}

static bool dsr_backup_slot(const wchar_t *src_save, int slot, const wchar_t *dst_backup, int level) {
    dsr_save_data_t *save = NULL;
    dsr_char_data_t *char_data;
    uint8_t *buf;
    bool ok;

    if (!dsr_save_data_load(src_save, &save)) return false;

    char_data = dsr_char_data_ref(save, slot);
    if (!char_data) {
        dsr_save_data_free(save);
        return false;
    }

    /* this value MUST match dsr_char_data_serialize's expected out_size */
    buf = LocalAlloc(LMEM_FIXED, DSR_CHAR_DATA_SERIALIZED_SIZE);
    if (!buf) {
        dsr_save_data_free(save);
        return false;
    }

    ok = dsr_char_data_serialize(char_data, buf, DSR_CHAR_DATA_SERIALIZED_SIZE);
    dsr_save_data_free(save);
    if (!ok) {
        LocalFree(buf);
        return false;
    }

    ok = ersm_compress_to_file(dst_backup, buf, DSR_CHAR_DATA_SERIALIZED_SIZE, ERSM_TYPE_CHAR_SLOT, level);
    LocalFree(buf);
    return ok;
}

static bool dsr_restore_slot(const wchar_t *src_backup, const wchar_t *dst_save, int slot) {
    uint8_t *buf;
    size_t buf_size = 0;
    uint8_t data_type = 0;
    dsr_save_data_t *save = NULL;
    bool ok;

    buf = ersm_decompress_from_file(src_backup, &buf_size, &data_type);
    if (!buf) return false;

    if (data_type != ERSM_TYPE_CHAR_SLOT || buf_size != DSR_CHAR_DATA_SERIALIZED_SIZE) {
        LocalFree(buf);
        return false;
    }

    if (!dsr_save_data_load(dst_save, &save)) {
        LocalFree(buf);
        return false;
    }

    ok = dsr_char_data_import_raw(save, slot, buf, buf_size);
    LocalFree(buf);
    dsr_save_data_free(save);
    return ok;
}

const game_backend_t dsr_backend = {
    .id = GAME_ID_DARK_SOULS_REMASTERED,
    .display_name = L"Dark Souls Remastered",
    .backup_extension = L".dsrsave",
    .save_filename = L"DRAKS0005.sl2",
    .needs_game_restart = false,
    .full_save_skip_compression = true,
    .resolve_save_path = dsr_resolve_save_path,
    .backup_full = dsr_backup_full,
    .restore_full = dsr_restore_full,
    .get_active_slot = dsr_get_active_slot,
    .backup_slot = dsr_backup_slot,
    .restore_slot = dsr_restore_slot,
    .get_default_save_dir = dsr_get_default_save_dir,
};
