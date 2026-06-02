/**
 * @file praxis_import_executor.h
 * @brief Batch import executor for save files.
 * @details Takes a list of scanned save files and performs the actual import
 *          according to the selected mode and the active profile's compression
 *          settings.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <windows.h>

#include "praxis_import_scanner.h"
#include "profile_store.h"
#include "save_tree.h"

typedef enum import_mode_e {
    IMPORT_MODE_ORIGINAL = 0,
    IMPORT_MODE_SINGLE_SLOT = 1
} import_mode_t;

/**
 * @brief Execute a batch import of selected save files.
 * @param hwnd       Parent window handle.
 * @param store      Active profile store.
 * @param save_tree  Save tree widget (used for target directory and refresh).
 * @param src_root   The root directory that was scanned (used to preserve relative subdir structure).
 * @param results    Scan result array.
 * @param selected   Bool array parallel to results; true means import this item.
 * @param count      Length of the arrays.
 * @param mode       Import mode (original format or force single slot).
 * @return Number of successfully imported files (>= 0).
 */
int praxis_import_execute(HWND hwnd, profile_store_t *store, save_tree_t *save_tree,
                          const wchar_t *src_root,
                          const import_scan_result_t *results, const bool *selected,
                          size_t count, import_mode_t mode);
