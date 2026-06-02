/**
 * @file praxis_import_scanner.c
 * @brief Recursive directory scanner for valid save files.
 * @details Walks a user-selected directory tree and collects files that
 *          are recognized as valid save backups by their magic bytes.
 *          Follows the same depth limit and skip rules as save_tree_walk.c.
 */

#include "praxis_import_scanner.h"

#include <stdint.h>
#include <wchar.h>

#include <windows.h>

static bool append_result(import_scan_list_t *list, const import_scan_result_t *result) {
    if (!list || !result) {
        return false;
    }

    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        size_t new_size = new_capacity * sizeof(import_scan_result_t);
        import_scan_result_t *new_items;

        new_items = (import_scan_result_t *)LocalAlloc(LMEM_FIXED, new_size);
        if (!new_items) {
            return false;
        }

        if (list->items && list->count > 0) {
            CopyMemory(new_items, list->items, list->count * sizeof(import_scan_result_t));
            LocalFree(list->items);
        }

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = *result;
    return true;
}

static void scan_dir_recursive(const wchar_t *dir_path, import_scan_list_t *out_list, int depth) {
    WIN32_FIND_DATAW find_data;
    HANDLE hfind;
    wchar_t search_path[MAX_PATH];

    if (!dir_path || !out_list || depth > IMPORT_SCAN_MAX_DEPTH) {
        return;
    }

    if (_snwprintf_s(search_path, MAX_PATH, _TRUNCATE, L"%ls\\*", dir_path) < 0) {
        return;
    }

    hfind = FindFirstFileW(search_path, &find_data);
    if (hfind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        const wchar_t *name = find_data.cFileName;

        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) {
            continue;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }

        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t sub_path[MAX_PATH];
            if (_snwprintf_s(sub_path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir_path, name) >= 0) {
                scan_dir_recursive(sub_path, out_list, depth + 1);
            }
        } else {
            wchar_t file_path[MAX_PATH];
            import_scan_result_t result = {0};
            save_kind_t kind;
            ersm_format_t fmt;
            LARGE_INTEGER file_size;

            if (_snwprintf_s(file_path, MAX_PATH, _TRUNCATE, L"%ls\\%ls", dir_path, name) < 0) {
                continue;
            }

            kind = save_compress_classify_backup(file_path);
            if (kind == SAVE_KIND_UNKNOWN) {
                continue;
            }

            fmt = ersm_detect_file_format(file_path);

            lstrcpynW(result.full_path, file_path, MAX_PATH);
            lstrcpynW(result.file_name, name, MAX_PATH);
            result.kind = kind;
            result.format = fmt;

            file_size.LowPart = find_data.nFileSizeLow;
            file_size.HighPart = find_data.nFileSizeHigh;
            result.file_size = (uint64_t)file_size.QuadPart;

            append_result(out_list, &result);
        }
    } while (FindNextFileW(hfind, &find_data));

    FindClose(hfind);
}

void import_scan_list_init(import_scan_list_t *list) {
    if (!list) {
        return;
    }
    ZeroMemory(list, sizeof(*list));
}

void import_scan_list_free(import_scan_list_t *list) {
    if (!list) {
        return;
    }
    if (list->items) {
        LocalFree(list->items);
        list->items = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

bool import_scan_directory(const wchar_t *root_path, import_scan_list_t *out_list) {
    if (!root_path || !out_list) {
        return false;
    }

    scan_dir_recursive(root_path, out_list, 0);
    return true;
}
