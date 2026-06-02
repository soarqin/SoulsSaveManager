/**
 * @file praxis_import_scanner.h
 * @brief Recursive directory scanner for valid save files.
 * @details Walks a user-selected directory tree and collects files that
 *          are recognized as valid save backups (full saves or slot saves)
 *          by their magic bytes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

#include "../common/save_compress.h"

#define IMPORT_SCAN_MAX_DEPTH 16

typedef struct import_scan_result_s {
    wchar_t full_path[MAX_PATH];
    wchar_t file_name[MAX_PATH];
    save_kind_t kind;
    ersm_format_t format;
    uint64_t file_size;
} import_scan_result_t;

typedef struct import_scan_list_s {
    import_scan_result_t *items;
    size_t count;
    size_t capacity;
} import_scan_list_t;

/**
 * @brief Initialize an empty scan list.
 */
void import_scan_list_init(import_scan_list_t *list);

/**
 * @brief Free the items array in a scan list.
 */
void import_scan_list_free(import_scan_list_t *list);

/**
 * @brief Recursively scan a directory for valid save files.
 * @param root_path  Wide-character path to the directory to scan.
 * @param out_list   Receives the list of valid save files found.
 * @return true on success (even if no files were found); false on I/O error.
 */
bool import_scan_directory(const wchar_t *root_path, import_scan_list_t *out_list);
