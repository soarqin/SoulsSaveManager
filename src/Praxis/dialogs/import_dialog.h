/**
 * @file import_dialog.h
 * @brief Import save files selection dialog.
 * @details Shows a ListView with checkboxes so the user can pick which
 *          scanned save files to import.
 */

#pragma once

#include <windows.h>

#include "../praxis_import_scanner.h"

/**
 * @brief Show the import selection dialog.
 * @param hwnd         Parent window handle.
 * @param results      Scan result array (input).
 * @param count        Array length.
 * @param out_selected Output bool array (caller must allocate count elements).
 * @return IDOK if user clicked Import, IDCANCEL if cancelled.
 */
int dialog_import_show(HWND hwnd, const import_scan_result_t *results, size_t count,
                       bool *out_selected);
